package com.ivanna.omega.audio

import android.content.Context
import android.util.Log
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.ui.AdaptiveControlsPrefs
import kotlin.math.log10

/**
 * EqStack — UNA SOLA SUMA DE EQ CON TOPE (unificación espacial 2.5.509,
 * paso 4).
 *
 * EL PROBLEMA (verificado): al subir HOLOS, la EQ se apilaba ~17 dB por
 * banda (Γ de Esse +7.8/−8.1/−8.6 + EQ de usuario 2.2/0.8/1.8 + trim Φ +
 * legacy +0.67 dB) contra el techo APEX 0.9885531, que es RECORTE DURO
 * (dos pasadas de std::clamp, sin ataque/release): la suma se convertía
 * en distorsión. La suma vivía en 4 sitios (FusedControls, JNI,
 * daemon, telemetría) sin tope común.
 *
 * LA SUMA (por banda, misma definición que la telemetría EQ_STACK_TAG):
 *  - graves:  Γ.low  + usuario.eqBass   + trim Φ + legacy
 *  - medios:  Γ.mid  + usuario.eqMid    + legacy
 *  - agudos:  Γ.high + presencia + usuario.eqTreble + legacy
 *  (legacy = 20·log10(masterGain); trim Φ = getHyperBassTrimDb, ±3 dB.)
 *
 * EL TOPE: cada suma se recorta a ±6 dB. El exceso SE DESCARTA (no se
 * aplica como boost en ningún sitio): la nota del informe «el exceso va
 * al trim de Φ» NO es aplicable banda a banda (el trim Φ es solo graves
 * ±3 dB), así que el tope recorta la contribución Γ. Si los OTROS
 * sumandos ya superan ±6 dB solos, Γ no puede compensarlo (límite
 * ±12 dB por banda, el valor acordado — ver abajo) y el exceso queda
 * registrado en el log, no escondido.
 *
 * RECONCILIACIÓN DEL TRIM (hallazgo de la lectura, 2.5.509): son TRES
 * trims distintos, cada uno correcto en su ruta — NO se cambia ningún
 * valor:
 *  (a) TonalControls.trimDb ±24 dB: compensador maestro P8, Ruta A
 *      fusionada. Bajarlo rompería la compensación Matched-LUFS, que
 *      necesita +14.18 dB. NO es un trim de banda.
 *  (b) auto-trim de loudness legacy ±12 dB (ivanna_omega_jni.cpp:2821).
 *  (c) trim EQ legacy ±12 dB (IvannaFusionCore.h:592).
 * El valor acordado para trims DE BANDA es ±12 dB (las dos rutas
 * coinciden); el tope ±6 dB de aquí es una capa APARTE sobre la SUMA.
 *
 * APLICACIÓN (idempotente, en los dos bordes de escritura): el tope se
 * aplica en safeSetFusedControls (JNI, Ruta A) y en pushFusedToDaemon
 * (daemon, Ruta B). Sin contexto (tests) no hay cap (los valores pasan
 * como hoy; buildFusedPerceptualFields sigue puro y testeable).
 */
object EqStack {
    private const val TAG = "IVANNA.EqStack"

    /** Tope de la SUMA por banda, en dB. */
    const val CAP_DB = 6.0f

    /** Límite de la contribución Γ por banda (valor acordado, ±12 dB). */
    const val GAMMA_CLAMP_DB = 12.0f

    /** Las tres fuentes de la suma, cargadas de prefs (null = sin contexto). */
    data class Inputs(
        val userLowDb: Float,
        val userMidDb: Float,
        val userHighDb: Float,
        val trimPhiDb: Float,
        val legacyDb: Float,
    )

    /** Resultado del tope: Γ recortada + sumas efectivas + exceso descartado. */
    data class Capped(
        val lowDb: Float,
        val midDb: Float,
        val highDb: Float,
        val presenceDb: Float,
        val capped: Boolean,
        val stackLowDb: Float,
        val stackMidDb: Float,
        val stackHighDb: Float,
        val excessLowDb: Float,
        val excessMidDb: Float,
        val excessHighDb: Float,
    )

    /** Carga las fuentes de la suma; null si no hay contexto (tests). */
    fun inputs(context: Context?): Inputs? {
        if (context == null) return null
        return runCatching {
            val user = AdaptiveControlsPrefs.load(context)
            val legacy = 20f * log10(user.masterGain.coerceAtLeast(1e-6f))
            val phi = runCatching { ParameterStore(context).getHyperBassTrimDb() }
                .getOrDefault(0f)
            Inputs(
                userLowDb = user.eqBass,
                userMidDb = user.eqMid,
                userHighDb = user.eqTreble,
                trimPhiDb = phi,
                legacyDb = legacy,
            )
        }.onFailure { Log.w(TAG, "EqStack.inputs falló; sin tope: ${it.message}") }
            .getOrNull()
    }

    /** Tope sobre TonalControls (borde JNI, Ruta A). Idempotente. */
    fun cap(tonal: TonalControls, inputs: Inputs?): TonalControls {
        if (inputs == null) return tonal
        val r = compute(tonal.lowDb, tonal.midDb, tonal.highDb, tonal.presenceDb, inputs)
        if (r.capped) {
            Log.i(
                TAG,
                "EqStack CAPPED: Γ low=${tonal.lowDb}→${r.lowDb} mid=${tonal.midDb}→${r.midDb} " +
                    "high=${tonal.highDb}→${r.highDb} (exceso ${r.excessLowDb}/${r.excessMidDb}/${r.excessHighDb} dB descartado)"
            )
        }
        return tonal.copy(lowDb = r.lowDb, midDb = r.midDb, highDb = r.highDb)
    }

    /** Tope sobre un mapa de payload (borde daemon, Ruta B). Idempotente. */
    fun capFields(fields: Map<String, Float>, inputs: Inputs?): Map<String, Float> {
        if (inputs == null) return fields
        if (!fields.containsKey("holosEqLow")) return fields
        val r = compute(
            gammaLow = fields["holosEqLow"] ?: 0f,
            gammaMid = fields["holosEqMid"] ?: 0f,
            gammaHigh = fields["holosEqHigh"] ?: 0f,
            gammaPresence = fields["holosEqPresence"] ?: 0f,
            inputs = inputs,
        )
        if (!r.capped) return fields
        return fields.toMutableMap().apply {
            put("holosEqLow", r.lowDb)
            put("holosEqMid", r.midDb)
            put("holosEqHigh", r.highDb)
        }
    }

    /** Núcleo puro: recorta Γ para que cada suma quede en ±[CAP_DB]. */
    fun compute(
        gammaLow: Float,
        gammaMid: Float,
        gammaHigh: Float,
        gammaPresence: Float,
        inputs: Inputs,
    ): Capped {
        val (lo, exLo) = capBand(gammaLow, inputs.userLowDb + inputs.trimPhiDb + inputs.legacyDb)
        val (mid, exMid) = capBand(gammaMid, inputs.userMidDb + inputs.legacyDb)
        val (hi, exHi) = capBand(gammaHigh, gammaPresence + inputs.userHighDb + inputs.legacyDb)
        return Capped(
            lowDb = lo,
            midDb = mid,
            highDb = hi,
            presenceDb = gammaPresence,
            capped = exLo != 0f || exMid != 0f || exHi != 0f,
            stackLowDb = lo + inputs.userLowDb + inputs.trimPhiDb + inputs.legacyDb,
            stackMidDb = mid + inputs.userMidDb + inputs.legacyDb,
            stackHighDb = hi + gammaPresence + inputs.userHighDb + inputs.legacyDb,
            excessLowDb = exLo,
            excessMidDb = exMid,
            excessHighDb = exHi,
        )
    }

    private fun capBand(gamma: Float, others: Float): Pair<Float, Float> {
        val sum = gamma + others
        val cappedSum = sum.coerceIn(-CAP_DB, CAP_DB)
        val gammaCapped = (cappedSum - others).coerceIn(-GAMMA_CLAMP_DB, GAMMA_CLAMP_DB)
        return gammaCapped to (sum - cappedSum)
    }
}
