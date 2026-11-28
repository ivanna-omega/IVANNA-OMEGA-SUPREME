package com.ivanna.omega.organism

import com.google.gson.Gson
import com.google.gson.JsonArray
import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.google.gson.JsonSyntaxException
import com.ivanna.omega.audio.IvannaAudioProfile
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
 *
 * C8 (release 510): diario de aplicación + estable con SHA-256
 * ([applyAndCommit], [checkPendingRollback], [restoreStableCandidate]).
 * El diario marca PENDING al instalar el candidato y solo pasa a
 * COMMITTED tras la lectura de vuelta del perfil aplicado; al arrancar,
 * `OrganismController.start()` restaura el estable verificado si el
 * diario quedó PENDING (máximo 1 intento por arranque).
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

    // ------------------------------------------------------------------
    // C8: diario de aplicación + estable verificado + rollback.
    //
    // El flujo multi-paso de la VUELTA (candidato → saveOverride() →
    // applyProfile("muselink-live") → motor) puede morir a mitad: el
    // diario deja constancia de lo que quedó a medias y el estable con
    // su SHA-256 es el segundo nivel de rollback de T1. La traducción al
    // DSP y el applyProfile NO viven aquí (eso es T1): [applyAndCommit]
    // recibe el paso de instalación como callback y solo marca COMMITTED
    // tras leer de vuelta el perfil aplicado.
    // ------------------------------------------------------------------

    /** Estado del diario de aplicación (visible; lo lee OrganismController al arrancar). */
    enum class JournalState { PENDING, COMMITTED, ROLLED_BACK }

    /**
     * Una entrada del diario `muselink-apply-journal.json`.
     * T1/510: [profileId] es el id omega del perfil instalado (lo usa el
     * rollback para registrar a qué perfil se vuelve; "" en diarios
     * escritos antes de T1) y [stableVersion] es la versión del archivo
     * estable vigente (el diario de la instalación NO es la versión del
     * estable: un candidato commiteado deja el diario en su versión pero
     * el estable sigue siendo la versión bendecida anterior; sin este
     * campo el rollback no sabría a qué versión volver).
     */
    data class ApplyJournal(
        val version: Long,
        val hashSha256: String,
        val estado: JournalState,
        val atMs: Long,
        val profileId: String = "",
        val stableVersion: Long? = null
    )

    sealed class CommitResult {
        /** Validado, instalado y confirmado por lectura de vuelta; estable promovido. */
        data class Committed(val profile: ValidatedProfile) : CommitResult()

        /** Validado pero NO confirmado: el diario queda PENDING y el estable intacto. */
        data class NotCommitted(val profile: ValidatedProfile, val reason: String) : CommitResult()

        /** Rechazado y puesto en cuarentena; nunca aplicado. */
        data class Rejected(
            val validation: ValidationResult.Invalid,
            val quarantineFile: File
        ) : CommitResult()
    }

    sealed class RollbackCheck {
        /** Sin diario PENDING: nada que restaurar. */
        data object None : RollbackCheck()

        /** Diario PENDING y estable presente con hash verificado: listo para restaurar. */
        data class Ready(val version: Long, val stableBody: String) : RollbackCheck()

        /** Diario PENDING pero no hay estable en disco. */
        data class StableMissing(val version: Long) : RollbackCheck()

        /** Diario PENDING pero el estable NO verifica su hash: NO se restaura. */
        data class StableHashMismatch(
            val version: Long,
            val expectedHash: String,
            val actualHash: String
        ) : RollbackCheck()
    }

    /**
     * Punto de entrada para T1: valida, instala con diario y confirma por
     * lectura de vuelta. Orden exacto:
     *   1. valida (inválido → cuarentena, [CommitResult.Rejected]);
     *   2. escribe el candidato (`muselink-profile-<version>.ivanna.json`,
     *      tmp+rename) + diario `{version, hashSha256, estado: PENDING}`;
     *   3. llama a [install]: T1 traduce a `IvannaAudioProfile`, hace
     *      `saveOverride()` + `applyProfile("muselink-live", …)`. Si
     *      devuelve false, el diario queda PENDING y se devuelve
     *      [CommitResult.NotCommitted] (el estable queda intacto);
     *   4. lee de vuelta el estado aplicado con [readAppliedProfileId]
     *      (T1 lo cablea a `ProfileManagerBridge.getCurrentProfileId()`;
     *      el snapshot del daemon no lleva id de perfil, ver reporte C8).
     *      Solo si devuelve `"muselink-live"` se marca el diario
     *      COMMITTED y se promueve el estable (archivo + SHA-256).
     *      Cualquier otro valor (o null) deja el diario PENDING para que
     *      el próximo arranque restaure el estable.
     *
     * T1/510: [profileId] es el id omega del sobre (queda en el diario
     * para el rollback) y [promoteToStable] decide si este commit
     * promueve el archivo estable. Un candidato (`candidate` /
     * `ab_running`) se commitea SIN promover: el estable sigue siendo la
     * versión bendecida anterior, que es el destino del rollback si el
     * servidor marca `rolled_back`. Un estable o un rollback restaurado
     * sí promueven. El diario registra en ambos casos la versión del
     * estable vigente en [ApplyJournal.stableVersion].
     */
    fun applyAndCommit(
        body: String,
        version: Long,
        appliedVersion: Long?,
        rolledBack: Boolean = false,
        previous: ValidatedProfile? = null,
        profileId: String = "",
        promoteToStable: Boolean = true,
        install: (ValidatedProfile) -> Boolean,
        readAppliedProfileId: () -> String?
    ): CommitResult {
        val validated = when (val result = validate(body, version, appliedVersion, rolledBack, previous)) {
            is ValidationResult.Valid -> result.profile
            is ValidationResult.Invalid -> {
                val file = quarantine(body, version, result.reasons)
                return CommitResult.Rejected(result, file)
            }
        }
        installCandidate(body, version, profileId)
        if (!install(validated)) {
            return CommitResult.NotCommitted(validated, "install() devolvió false; diario queda PENDING")
        }
        val appliedId = runCatching { readAppliedProfileId() }.getOrNull()
        if (appliedId != LIVE_PROFILE_ID) {
            return CommitResult.NotCommitted(
                validated,
                "read-back: perfil aplicado '$appliedId', se esperaba '$LIVE_PROFILE_ID'; diario queda PENDING"
            )
        }
        val prevStableVersion = readJournal()?.stableVersion
        if (promoteToStable) {
            promoteStable(body, version)
        }
        writeJournal(
            ApplyJournal(
                version,
                sha256Hex(body),
                JournalState.COMMITTED,
                clockMs(),
                profileId,
                stableVersion = if (promoteToStable) version else prevStableVersion
            )
        )
        pruneCandidates(keepVersion = null)
        return CommitResult.Committed(validated)
    }

    /**
     * Escribe el candidato con tmp+rename y el diario en PENDING con el
     * SHA-256 del cuerpo. Conserva solo estable + este candidato + diario.
     * El diario PENDING conserva la versión del estable vigente (si la
     * había): el candidato aún no es estable.
     */
    fun installCandidate(body: String, version: Long, profileId: String = ""): File {
        dir.mkdirs()
        val file = File(dir, candidateFileName(version))
        writeAtomic(file, body)
        val prevStableVersion = readJournal()?.stableVersion
        writeJournal(ApplyJournal(version, sha256Hex(body), JournalState.PENDING, clockMs(), profileId, prevStableVersion))
        pruneCandidates(keepVersion = version)
        return file
    }

    /** Diario actual, o null si nunca se escribió uno o no parsea. */
    fun readJournal(): ApplyJournal? {
        val file = File(dir, JOURNAL_FILE_NAME)
        if (!file.isFile) return null
        return runCatching {
            val obj = gson.fromJson(file.readText(), JsonObject::class.java)
            ApplyJournal(
                version = obj.get("version").asLong,
                hashSha256 = obj.get("hashSha256").asString,
                estado = JournalState.valueOf(obj.get("estado").asString),
                atMs = obj.get("atMs").asLong,
                // T1/510: tolerante con diarios escritos antes de T1 (sin
                // estos campos): profileId "" y stableVersion null.
                profileId = obj.get("profileId")?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }?.asString ?: "",
                stableVersion = obj.get("stableVersion")?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isNumber }?.asLong
            )
        }.getOrNull()
    }

    fun writeJournal(journal: ApplyJournal) {
        dir.mkdirs()
        val obj = JsonObject().apply {
            addProperty("version", journal.version)
            addProperty("hashSha256", journal.hashSha256)
            addProperty("estado", journal.estado.name)
            addProperty("atMs", journal.atMs)
            addProperty("profileId", journal.profileId)
            journal.stableVersion?.let { addProperty("stableVersion", it) }
        }
        writeAtomic(File(dir, JOURNAL_FILE_NAME), gson.toJson(obj))
    }

    /**
     * Promueve el cuerpo a estable: escribe `muselink-profile-stable.ivanna.json`
     * (tmp+rename) + su SHA-256 en el sidecar `.sha256`, y verifica el hash
     * tras escribir. Devuelve true solo si la verificación posterior pasa.
     */
    fun promoteStable(body: String, version: Long): Boolean {
        dir.mkdirs()
        writeAtomic(File(dir, STABLE_FILE_NAME), body)
        writeAtomic(File(dir, STABLE_HASH_FILE_NAME), sha256Hex(body))
        return verifyStableHash()
    }

    /** True solo si el estable existe, tiene sidecar `.sha256` y el hash coincide. */
    fun verifyStableHash(): Boolean {
        val stable = File(dir, STABLE_FILE_NAME)
        val hashFile = File(dir, STABLE_HASH_FILE_NAME)
        if (!stable.isFile || !hashFile.isFile) return false
        val expected = runCatching { hashFile.readText().trim() }.getOrNull() ?: return false
        val actual = runCatching { sha256Hex(stable.readText()) }.getOrNull() ?: return false
        return expected.equals(actual, ignoreCase = true)
    }

    /** Cuerpo del estable solo si su hash verifica; null en cualquier otro caso. */
    fun readStableBody(): String? {
        if (!verifyStableHash()) return null
        return runCatching { File(dir, STABLE_FILE_NAME).readText() }.getOrNull()
    }

    /**
     * Lo que lee `OrganismController.start()`: si el diario está PENDING,
     * decide si el estable puede restaurarse. Un hash distinto (estable
     * corrupto o manipulado) devuelve [RollbackCheck.StableHashMismatch]
     * y NUNCA restaura: mejor quedarse con el último bueno en el motor
     * que aplicar un estable no verificado.
     */
    fun checkPendingRollback(): RollbackCheck {
        val journal = readJournal() ?: return RollbackCheck.None
        if (journal.estado != JournalState.PENDING) return RollbackCheck.None
        val stable = File(dir, STABLE_FILE_NAME)
        if (!stable.isFile) return RollbackCheck.StableMissing(journal.version)
        val body = readStableBody()
        if (body == null) {
            val expected = runCatching { File(dir, STABLE_HASH_FILE_NAME).readText().trim() }.getOrNull() ?: ""
            val actual = runCatching { sha256Hex(stable.readText()) }.getOrNull() ?: ""
            return RollbackCheck.StableHashMismatch(journal.version, expected, actual)
        }
        return RollbackCheck.Ready(journal.version, body)
    }

    /**
     * Restaura el estable verificado como candidato de la versión pendiente
     * (tmp+rename), para que el próximo intento de T1 re-aplique el estable
     * y no el candidato a medias. El diario queda PENDING a propósito:
     * solo T1 lo marca COMMITTED (tras re-aplicar + read-back) o
     * ROLLED_BACK (vía [completeRollback]). Devuelve false si no había
     * nada que restaurar o el hash no verificó.
     */
    fun restoreStableCandidate(): Boolean {
        val check = checkPendingRollback()
        if (check !is RollbackCheck.Ready) return false
        writeAtomic(File(dir, candidateFileName(check.version)), check.stableBody)
        return true
    }

    /** Marca el diario ROLLED_BACK tras re-aplicar el estable con read-back ok (lo llama T1). */
    fun completeRollback(version: Long) {
        val journal = readJournal() ?: return
        if (journal.version != version || journal.estado != JournalState.PENDING) return
        writeJournal(journal.copy(estado = JournalState.ROLLED_BACK, atMs = clockMs()))
    }

    /**
     * Tope de archivos (C8): solo estable + candidato vigente + diario.
     * Borra candidatos viejos (`muselink-profile-<n>.ivanna.json`) y sus
     * `.tmp` huérfanos. La cuarentena no se toca (otro prefijo).
     */
    private fun pruneCandidates(keepVersion: Long?) {
        val files = dir.listFiles() ?: return
        for (f in files) {
            val name = f.name
            val match = CANDIDATE_FILE_REGEX.matchEntire(name) ?: continue
            val v = match.groupValues[1].toLongOrNull()
            if (v != keepVersion) f.delete()
            val tmp = File(dir, "$name.tmp")
            if (tmp.isFile) tmp.delete()
        }
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

        /** Sidecar con el SHA-256 del estable; se verifica antes de cada rollback (C8). */
        const val STABLE_HASH_FILE_NAME = "muselink-profile-stable.ivanna.json.sha256"

        /** Diario de aplicación multi-paso (C8): {version, hashSha256, estado}. */
        const val JOURNAL_FILE_NAME = "muselink-apply-journal.json"

        /** Id del perfil vivo en el motor; el read-back debe devolver exactamente este valor. */
        const val LIVE_PROFILE_ID = "muselink-live"

        /**
         * Sala de escucha de Esse (verificado 2026-11-21): 4.0 m de ancho
         * × 7.0 m de largo, escucha a 3 m. La VUELTA §3.2 fija estos tres
         * valores para todo perfil descargado (el `.ivanna.json` no trae
         * sala: SalaViva no tiene slot en el engine v1.0).
         */
        const val SALA_RT60_SEC = 0.34f
        const val SALA_ROOM_SIZE_M = 7.0f
        const val SALA_ROOM_WET = 0.22f

        /**
         * T1/510 — traducción del [ValidatedProfile] al esquema interno
         * [IvannaAudioProfile] (VUELTA §3.2 paso 2). Pura y testeable en
         * JVM: no toca disco, DSP ni Android.
         *
         * MAPEO DUAL VERIFICADO (ver `510-prep/T1-double-eq-verification.md`):
         * el EQ se escribe en AMBOS destinos con el MISMO valor, sin
         * dividir entre 2: `audioEngine.eqXDb` (EQ legacy, suena en rutas
         * legacy / `nativeProcessBlock`) y `tonal.xDb` (EQ fusionado
         * ARMONÍA, suena en las rutas fusionadas vivas). `pushToNative()`
         * y el tonal fusionado alimentan dos EQs distintos detrás de
         * ramas mutuamente excluyentes; escribir en un solo destino
         * dejaría al EQ mudo en la otra ruta. La presencia se escribe en
         * ambos destinos aunque hoy se pierda en las rutas fusionadas
         * (sin slot JNI hasta S3 — el slot ya existe en la ola 1).
         *
         * HERENCIA: lo que el `.ivanna.json` no cubre (`audioEngine.gain`,
         * `widthAmount`, `antiDolby`, `neuromorphic`, `route`, resto de
         * `spatial`/`field`/`volterra`/`sala`/`tonal`/`dynamics`/`brain`/
         * `output`) se hereda de [base] (el perfil que se reemplaza);
         * nunca se inventa. [base] lo resuelve el controlador (muselink-live
         * vigente, o el perfil aplicado, o el primero de la lista).
         *
         * HUECOS DOCUMENTADOS (no se inventan):
         * - `volterra_mix` (0.55) no tiene slot en el esquema interno: se
         *   conserva en el `.ivanna.json` y no se aplica.
         *   `volterra.correction` es solo on/off (1.0 si `volterra_enabled`,
         *   0.0 si no), igual que `TransducerControls.correction`
         *   («cantidad de corrección 0..1»). Nota: `IvannaOmegaFile`
         *   mapea el mix como correction; la VUELTA especifica on/off y
         *   manda la especificación aquí.
         * - `structural_split` no tiene operador en el DSP (diseño aparte);
         *   se valida pero no se traduce.
         * - `profileId` null (solo en el camino de rollback, cuando el
         *   diario no lo registró) omite el id de la descripción; el
         *   resto del mapeo no cambia.
         */
        fun translate(
            validated: ValidatedProfile,
            status: String,
            profileId: String?,
            base: IvannaAudioProfile
        ): IvannaAudioProfile {
            // 1. Calcular UNA vez por banda, mismo valor para ambos destinos.
            val low = validated.eqBandsDb.low60_120.toFloat()
            val mid = validated.eqBandsDb.mid500_1k.toFloat()
            val high = validated.eqBandsDb.high2k_4k.toFloat()
            val presence = validated.eqBandsDb.presence4k_8k.toFloat()
            val description = if (profileId.isNullOrBlank()) {
                "MuseLink · ${validated.source} · $status"
            } else {
                "MuseLink · ${validated.source} · $status · $profileId"
            }
            return base.copy(
                id = LIVE_PROFILE_ID,
                name = "${validated.track} (v${validated.version})",
                description = description,
                category = "muselink",
                priority = base.priority,
                audioEngine = base.audioEngine.copy(
                    exciterAmount = validated.exciterAmount.toFloat(),
                    // Mapeo dual: mismo valor en el destino legacy…
                    eqLowDb = low,
                    eqMidDb = mid,
                    eqHighDb = high,
                    eqPresenceDb = presence
                    // gain / eqGain / widthAmount / bypass: heredados de base.
                ),
                spatial = base.spatial.copy(
                    orbeAmount = validated.orbeAmount.toFloat(),
                    orbeDistanceM = validated.orbeDistanceM.toFloat()
                ),
                field = base.field.copy(
                    holosAmount = validated.holosAmount.toFloat(),
                    phiAmount = validated.phiAmount.toFloat()
                ),
                volterra = base.volterra.copy(
                    correction = if (validated.volterraEnabled) 1.0f else 0.0f
                ),
                sala = base.sala.copy(
                    rt60Sec = SALA_RT60_SEC,
                    roomSizeM = SALA_ROOM_SIZE_M,
                    roomWet = SALA_ROOM_WET
                ),
                tonal = base.tonal.copy(
                    trimDb = validated.trimDb.toFloat(),
                    // …y en el destino fusionado (mismo valor, sin /2).
                    lowDb = low,
                    midDb = mid,
                    highDb = high,
                    presenceDb = presence
                ),
                dynamics = base.dynamics.copy(
                    compThresholdDb = validated.compressor.thresholdDb.toFloat(),
                    compRatio = validated.compressor.ratio.toFloat()
                ),
                tags = listOf("muselink-live", status),
                referenceTrack = validated.track,
                targetLufsDeltaDb = validated.netGainDb.toFloat()
                // antiDolby / neuromorphic / route / brain / output /
                // resto de spatial/field/volterra/sala/tonal/dynamics:
                // heredados de base (copy los conserva).
            )
        }

        /** SHA-256 hex (minúsculas) del cuerpo; usado por diario y sidecar del estable. */
        fun sha256Hex(text: String): String {
            val digest = java.security.MessageDigest.getInstance("SHA-256")
                .digest(text.toByteArray(Charsets.UTF_8))
            return digest.joinToString("") { "%02x".format(it) }
        }

        const val QUARANTINE_STATUS_FILE_NAME = "muselink-profile-quarantine.json"

        fun candidateFileName(version: Long): String = "muselink-profile-$version.ivanna.json"

        /** Solo candidatos versionados (`muselink-profile-<n>.ivanna.json`); nunca el estable ni la cuarentena. */
        val CANDIDATE_FILE_REGEX = Regex("""muselink-profile-(\d+)\.ivanna\.json""")

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
