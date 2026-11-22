package com.ivanna.omega.dsp

import android.content.Context
import android.util.Log
import androidx.compose.runtime.Immutable
import com.ivanna.omega.magisk.OmegaEngineBridge
import org.json.JSONObject

/**
 * FÍSICA NUEVA (Fase 2, v2.5.472) — Puente Kotlin <-> JNI + persistencia.
 * Estado reactivo, persistencia local en SharedPreferences y calibración
 * del Sony Genezi. Default MAGISTRAL (v2.5.479, calibración 2026-11-20):
 * amount=1.0, sigma=3.5 m, xtalk=1.0 — probado en vivo por Esse, sin
 * piiiii (el piiiii fue el sigma 15.79 m, no el NP).
 *
 * v2.5.483 — ÓRGANOS v3 DESPIERTOS: el estado suma las 6 etapas modales
 * sub-graves (la 0 es el diferencial Ω automático y no se edita), la mezcla
 * de cola de sala, la mezcla H3 y el RIR elegido. Los órganos v3 viajan
 * SOLO a Ruta A por JNI directo: el snapshot ABI v15 no tiene campos para
 * ellos y la Ruta B suena con los defaults compilados (np483_defaults.hpp)
 * hasta la extensión ABI. La tarjeta lo dice en pantalla.
 *
 * Cableado real: NewPhysicsStore.pushToNative -> DSPBridge.setNewPhysicsParams
 * -> JNI nativeSetNewPhysicsParams -> atomics g_np_* -> NewPhysicsEngine
 * (Ruta A) + pushToDaemon -> SET_PERCEPTUAL_STATE -> daemon -> snapshot
 * ABI v14 (OmegaDspSnapshot.np_*, omega_control_bus.h) -> efecto (Ruta B).
 * La Ruta B recibe los 17 controles desde que la app los empuja (este
 * archivo); que el efecto los aplique depende de que el daemon parsee las
 * claves np_* y las publique en el snapshot v14 (lado daemon, fuera de
 * este archivo). La tarjeta UI lo dice en pantalla; ningún control finge
 * más de lo que hace.
 */
@Immutable
data class SubBassStageState(
    val freqHz: Double = 68.5,
    val q: Double = 3.0,
    val gainDb: Double = 0.0,
    val lowShelf: Boolean = false,
    val active: Boolean = false
)

/** Defaults v2.5.483 (np483_defaults.hpp): polo Genezi 68.5 Hz + 4 modos
 * axiales de la sala rir_0101 (3.5 x 2.9 x 4.6 m). Cut-only (Fase 4). */
fun defaultSubBassStages(): List<SubBassStageState> = listOf(
    SubBassStageState(freqHz = 68.5, q = 3.0, gainDb = -3.0, active = true),
    SubBassStageState(freqHz = 37.3, q = 4.0, gainDb = -4.0, active = true),
    SubBassStageState(freqHz = 49.0, q = 4.0, gainDb = -4.0, active = true),
    SubBassStageState(freqHz = 59.1, q = 4.0, gainDb = -4.0, active = true),
    SubBassStageState(freqHz = 74.6, q = 4.0, gainDb = -3.0, active = true)
)

@Immutable
data class NewPhysicsState(
    // Config MAGISTRAL por defecto (calibración 2026-11-20, Esse, en vivo):
    // NP 1.0 + sigma 3.5 m (distancia real al Genezi) + xtalk 1.0 suenan
    // perfectos y SIN piiiii. El piiiii de la 475 fue el sigma 15.79 m
    // (slider/clamp viejos admitían hasta 30/80 m), NO el NP en sí.
    val amount: Float = 1.0f,            // ON por defecto (magistral)
    val lgnPinnaMix: Float = 1.0f,
    val brightSpotDamp: Float = 0.65f,
    val torsoEchoMix: Float = 0.18f,
    val headRadiusM: Float = 0.0875f,
    val azimuthDeg: Float = 0.0f,
    val elevationDeg: Float = 0.0f,
    val eqClass: Int = 0,                // 0=E1 Audífonos, 1=E2 Bocina G85, 2=E3 BT, 3=E4 Sony Genezi
    val omegaSignatureMix: Float = 0.0f,
    val boostCapDb: Float = 3.0f,
    val crosstalkCompMix: Float = 1.0f,
    val sigmaDistanceM: Float = 3.5f,
    val splEstimateDb: Float = 85.0f,
    val airCompMix: Float = 0.0f,
    // v2.5.483: con el kernel Klippel aproximado ya cargado, la corrección
    // Volterra está ENCENDIDA por defecto (antes 0.0 = Klippel dormido
    // aunque hubiera kernel). La corrección es diminuta (pico 0.007) y
    // solo cancela la distorsión que la bocina añadiría; el usuario puede
    // bajarla a 0 con el slider. La migración de una sola vez en load()
    // aplica este 1.0 a instalaciones existentes (que traían 0.0 guardado).
    val volterraMix: Float = 1.0f,
    val freeEnergyMix: Float = 0.0f,
    val abxOverrideMode: Int = 0,        // 0=Normal, 1=Ancla A (Bypass), 2=Física Nueva B
    // ── ÓRGANOS v3 (v2.5.483) ─────────────────────────────────────────────
    // 5 etapas modales editables (la 0 es el diferencial Ω automático, no
    // se edita). Defaults = np483_defaults.hpp (polo Genezi 68.5 Hz + 4
    // modos axiales de la sala rir_0101). Cut-only (Fase 4).
    val subBassStages: List<SubBassStageState> = defaultSubBassStages(),
    // Mezcla de cola de sala 0..1 (0.5 = cola real sin ahogar el directo).
    val roomTailMix: Float = 0.5f,
    // Mezcla H3 0..1 (1.0 = corrección H3 completa, pico 0.007).
    val klippelH3Mix: Float = 1.0f,
    // RIR elegida en assets/ivanna_omega/rir/ (default rir_0101.wav: sala
    // 3.5 x 2.9 x 4.6 m, distancia 3.70 m — la más próxima a los 3.5 m
    // reales de Esse al Genezi; RT60 0.34 s).
    val roomRirFile: String = "rir_0101.wav"
) {
    /**
     * Clamp fail-closed, espejo exacto de
     * `ivanna::newphys::NewPhysicsControls::clamp()` (fused/
     * new_physics_controls.hpp): mismos rangos, mismo default ante
     * no-finito. Solo lo usa el empuje a Ruta B ([NewPhysicsStore.
     * buildNewPhysicsPerceptualFields]); la Ruta A (JNI) sigue recibiendo
     * los valores tal cual, como hasta hoy — no se cambia su comportamiento.
     */
    fun clamp(): NewPhysicsState = copy(
        // OVERDRIVE v2.5.480 (Esse): amount y xtalk admiten hasta 1.5.
        // 1.0 es el 100%; 1.5 extrapola la diferencia wet−dry ×1.5.
        amount = clampNp(amount, 0.0f, 1.5f, 1.0f),
        lgnPinnaMix = clampNp(lgnPinnaMix, 0.0f, 1.0f, 1.0f),
        brightSpotDamp = clampNp(brightSpotDamp, 0.0f, 1.0f, 0.65f),
        torsoEchoMix = clampNp(torsoEchoMix, 0.0f, 0.5f, 0.18f),
        headRadiusM = clampNp(headRadiusM, 0.065f, 0.115f, 0.0875f),
        azimuthDeg = clampNp(azimuthDeg, -180.0f, 180.0f, 0.0f),
        elevationDeg = clampNp(elevationDeg, -40.0f, 70.0f, 0.0f),
        // EquipmentClass C++ 0..6 (0 audífonos, 3 Genezi PZ1D); la UI solo
        // ofrece 0..3, pero el clamp admite el rango completo del motor.
        eqClass = eqClass.coerceIn(0, 6),
        omegaSignatureMix = clampNp(omegaSignatureMix, 0.0f, 1.0f, 0.0f),
        boostCapDb = clampNp(boostCapDb, 0.0f, 9.0f, 3.0f),
        crosstalkCompMix = clampNp(crosstalkCompMix, 0.0f, 1.5f, 1.0f),
        // Sigma: 0.15–5.0 m. El tope 2.0 m de la 479 bloqueaba la distancia
        // real de Esse (3.5 m al Genezi); 5.0 m la admite con margen y
        // sigue impidiendo el desastre del sigma 15.79 m (que daba el
        // piiiii). El motor C++ admite hasta 80 m; el clamp de la app es
        // la puerta que evita valores absurdos, no el límite del motor.
        sigmaDistanceM = clampNp(sigmaDistanceM, 0.15f, 5.0f, 3.5f),
        splEstimateDb = clampNp(splEstimateDb, 65.0f, 115.0f, 85.0f),
        airCompMix = clampNp(airCompMix, 0.0f, 1.0f, 0.0f),
        volterraMix = clampNp(volterraMix, 0.0f, 1.0f, 1.0f),
        freeEnergyMix = clampNp(freeEnergyMix, 0.0f, 1.0f, 0.0f),
        abxOverrideMode = abxOverrideMode.coerceIn(0, 2),
        // Órganos v3: cada etapa se clampa a su rango (freq 15..120 Hz,
        // Q 0.3..18, gain -12..+6 dB); fuera de rango o no-finito -> el
        // default de esa etapa. La lista siempre tiene 5 etapas.
        subBassStages = (0 until 5).map { i ->
            val d = defaultSubBassStages()[i]
            val s = subBassStages.getOrNull(i) ?: d
            SubBassStageState(
                freqHz = if (s.freqHz.isFinite()) s.freqHz.coerceIn(15.0, 120.0) else d.freqHz,
                q = if (s.q.isFinite()) s.q.coerceIn(0.3, 18.0) else d.q,
                gainDb = if (s.gainDb.isFinite()) s.gainDb.coerceIn(-12.0, 6.0) else d.gainDb,
                lowShelf = s.lowShelf,
                active = s.active
            )
        },
        roomTailMix = clampNp(roomTailMix, 0.0f, 1.0f, 0.5f),
        klippelH3Mix = clampNp(klippelH3Mix, 0.0f, 1.0f, 1.0f),
        roomRirFile = roomRirFile.takeIf { it.matches(Regex("rir_\\d{4}\\.wav")) }
            ?: "rir_0101.wav"
    )
}

/** clamp del C++ (`NewPhysicsControls::clamp`): no-finito → default, si no coerceIn. */
private fun clampNp(v: Float, lo: Float, hi: Float, def: Float): Float =
    if (!v.isFinite()) def else v.coerceIn(lo, hi)

@Immutable
data class NewPhysicsTelemetryUi(
    val sigErrorRmsDb: Float = 0f,
    val xtalkCeilingDb: Float = 99f,
    val scaleGamma: Float = 1f,
    val ildAlpha: Float = 1f,
    val mocFatigue: Float = 0f,
    val volterraPeak: Float = 0f,
    val cpuUsPerHop: Float = 0f,
    val kernelLoaded: Boolean = false
) {
    companion object {
        fun fromArray(a: FloatArray): NewPhysicsTelemetryUi {
            if (a.size < 8) return NewPhysicsTelemetryUi()
            return NewPhysicsTelemetryUi(
                sigErrorRmsDb = a[0],
                xtalkCeilingDb = a[1],
                scaleGamma = a[2],
                ildAlpha = a[3],
                mocFatigue = a[4],
                volterraPeak = a[5],
                cpuUsPerHop = a[6],
                kernelLoaded = a[7] >= 0.5f
            )
        }
    }
}

/** Telemetría v3 (v2.5.483): temperaturas de bobina, cola de sala, H3. */
@Immutable
data class NewPhysicsV3TelemetryUi(
    val coilTempL: Float = 25f,
    val coilTempR: Float = 25f,
    val roomTailActive: Boolean = false,
    val roomTailMix: Float = 0f,
    val h3Mix: Float = 1f,
    val subBassMask: Int = 0,
    val kernelLoaded: Boolean = false,
    val volterraPeak: Float = 0f,
    val cpuUsPerHop: Float = 0f,
    // true = kernel MEDIDO (applyCalibration / genezi_calibrate.py);
    // false = kernel APROXIMADO v2.5.483 (o sin kernel).
    val geneziKernelMeasured: Boolean = false
) {
    companion object {
        fun fromArray(a: FloatArray): NewPhysicsV3TelemetryUi {
            if (a.size < 10) return NewPhysicsV3TelemetryUi()
            return NewPhysicsV3TelemetryUi(
                coilTempL = a[0],
                coilTempR = a[1],
                roomTailActive = a[2] >= 0.5f,
                roomTailMix = a[3],
                h3Mix = a[4],
                subBassMask = a[5].toInt(),
                kernelLoaded = a[6] >= 0.5f,
                volterraPeak = a[7],
                cpuUsPerHop = a[8],
                geneziKernelMeasured = a[9] >= 0.5f
            )
        }
    }
}

/** Una RIR del dataset (assets/ivanna_omega/rir/metadata.csv). */
@Immutable
data class RirEntry(
    val file: String,
    val widthM: Double,
    val heightM: Double,
    val depthM: Double,
    val distanceM: Double,
    val rt60: Double
)

object NewPhysicsStore {
    private const val PREFS = "ivanna_new_physics_v1"
    private const val TAG = "NewPhysicsStore"

    fun save(ctx: Context, s: NewPhysicsState) {
        val e = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putFloat("amount", s.amount)
            .putFloat("lgnPinnaMix", s.lgnPinnaMix)
            .putFloat("brightSpotDamp", s.brightSpotDamp)
            .putFloat("torsoEchoMix", s.torsoEchoMix)
            .putFloat("headRadiusM", s.headRadiusM)
            .putFloat("azimuthDeg", s.azimuthDeg)
            .putFloat("elevationDeg", s.elevationDeg)
            .putInt("eqClass", s.eqClass)
            .putFloat("omegaSignatureMix", s.omegaSignatureMix)
            .putFloat("boostCapDb", s.boostCapDb)
            .putFloat("crosstalkCompMix", s.crosstalkCompMix)
            .putFloat("sigmaDistanceM", s.sigmaDistanceM)
            .putFloat("splEstimateDb", s.splEstimateDb)
            .putFloat("airCompMix", s.airCompMix)
            .putFloat("volterraMix", s.volterraMix)
            .putFloat("freeEnergyMix", s.freeEnergyMix)
            // Órganos v3 (v2.5.483): 5 etapas (idx 1..5) + cola + H3 + RIR.
            .putFloat("v3_tailMix", s.roomTailMix)
            .putFloat("v3_h3Mix", s.klippelH3Mix)
            .putString("v3_rirFile", s.roomRirFile)
            .putBoolean("v3OrgansSeen483", true)
        for ((i, st) in s.subBassStages.withIndex()) {
            val n = i + 1
            e.putFloat("v3_sub${n}_freq", st.freqHz.toFloat())
                .putFloat("v3_sub${n}_q", st.q.toFloat())
                .putFloat("v3_sub${n}_gain", st.gainDb.toFloat())
                .putBoolean("v3_sub${n}_shelf", st.lowShelf)
                .putBoolean("v3_sub${n}_active", st.active)
        }
        e.apply()
        // NOTA: abxOverrideMode NO se persiste a propósito — el ABX es de
        // sesión: cada arranque vuelve a Normal (0).
    }

    fun load(ctx: Context): NewPhysicsState {
        val p = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        // MIGRACIÓN v2.5.483 (una sola vez, marcada por v3OrgansSeen483):
        // despertar los órganos v3 en instalaciones existentes. Antes, el
        // volterraMix guardado era 0.0 (default viejo) y el Klippel nunca
        // corría aunque hubiera kernel; ahora el default es 1.0. Solo se
        // toca volterraMix: el resto del estado del usuario (NP, sigma,
        // xtalk, eqClass...) queda intacto. Si el usuario baja el slider
        // Volterra a 0, save() persiste su 0 y la migración no vuelve.
        if (!p.getBoolean("v3OrgansSeen483", false)) {
            p.edit().putBoolean("v3OrgansSeen483", true)
                .putFloat("volterraMix", 1.0f).apply()
        }
        // MIGRACIÓN v2.5.485 (una sola vez, marcada por rock70sSeen485):
        // deltas Rock 70s de NP/v3. Solo se tocan estos 3 campos: equipo
        // Genezi (E4), Bright-Spot 0.70 y cola 0.55. Todo lo demás se
        // conserva tal cual está guardado — incluido el sigma 0.35 del
        // teléfono (load() lee las prefs guardadas, no las sobrescribe).
        if (!p.getBoolean("rock70sSeen485", false)) {
            p.edit().putBoolean("rock70sSeen485", true)
                .putInt("eqClass", 3)
                .putFloat("brightSpotDamp", 0.70f)
                .putFloat("v3_tailMix", 0.55f).apply()
        }
        // v2.5.482: .clamp() al cargar — las prefs viejas podían traer un
        // sigma 15.79 m (o cualquier valor fuera de rango) y entrar a Ruta A
        // por JNI sin pasar por ningún clamp (el C++ admite hasta 80 m).
        // El clamp de la app (sigma 0.15–5.0 m) es la puerta que lo impide.
        val defaults = defaultSubBassStages()
        return NewPhysicsState(
            amount = p.getFloat("amount", 1.0f),
            lgnPinnaMix = p.getFloat("lgnPinnaMix", 1.0f),
            brightSpotDamp = p.getFloat("brightSpotDamp", 0.65f),
            torsoEchoMix = p.getFloat("torsoEchoMix", 0.18f),
            headRadiusM = p.getFloat("headRadiusM", 0.0875f),
            azimuthDeg = p.getFloat("azimuthDeg", 0.0f),
            elevationDeg = p.getFloat("elevationDeg", 0.0f),
            eqClass = p.getInt("eqClass", 0),
            omegaSignatureMix = p.getFloat("omegaSignatureMix", 0.0f),
            boostCapDb = p.getFloat("boostCapDb", 3.0f),
            crosstalkCompMix = p.getFloat("crosstalkCompMix", 1.0f),
            sigmaDistanceM = p.getFloat("sigmaDistanceM", 3.5f),
            splEstimateDb = p.getFloat("splEstimateDb", 85.0f),
            airCompMix = p.getFloat("airCompMix", 0.0f),
            volterraMix = p.getFloat("volterraMix", 1.0f),
            freeEnergyMix = p.getFloat("freeEnergyMix", 0.0f),
            subBassStages = (0 until 5).map { i ->
                val d = defaults[i]
                val n = i + 1
                SubBassStageState(
                    freqHz = p.getFloat("v3_sub${n}_freq", d.freqHz.toFloat()).toDouble(),
                    q = p.getFloat("v3_sub${n}_q", d.q.toFloat()).toDouble(),
                    gainDb = p.getFloat("v3_sub${n}_gain", d.gainDb.toFloat()).toDouble(),
                    lowShelf = p.getBoolean("v3_sub${n}_shelf", d.lowShelf),
                    active = p.getBoolean("v3_sub${n}_active", d.active)
                )
            },
            roomTailMix = p.getFloat("v3_tailMix", 0.5f),
            klippelH3Mix = p.getFloat("v3_h3Mix", 1.0f),
            roomRirFile = p.getString("v3_rirFile", "rir_0101.wav") ?: "rir_0101.wav"
        ).clamp()
    }

    // Órganos v3: el wake del JNI ya corre en prepare(), pero un
    // re-prepare posterior no re-despierta; este guarda de proceso lo
    // re-arme una vez por proceso (idempotente; si el nativo aún no está
    // cargado, queda en false y reintenta en el próximo empuje).
    @Volatile
    private var v3Woken = false

    fun pushToNative(s: NewPhysicsState) {
        DSPBridge.setNewPhysicsParams(
            s.amount, s.lgnPinnaMix, s.brightSpotDamp, s.torsoEchoMix, s.headRadiusM,
            s.azimuthDeg, s.elevationDeg, s.eqClass, s.omegaSignatureMix, s.boostCapDb,
            s.crosstalkCompMix, s.sigmaDistanceM, s.splEstimateDb, s.airCompMix,
            s.volterraMix, s.freeEnergyMix, s.abxOverrideMode
        )
        // ── ÓRGANOS v3 (v2.5.483, Ruta A por JNI directo) ─────────────────
        // NO van al daemon: el snapshot ABI v15 no tiene campos para ellos
        // (los 17 np_* sí viajan por pushToDaemon, abajo). La Ruta B suena
        // con los defaults compilados (np483_defaults.hpp) hasta la
        // extensión ABI. El wake carga los defaults; luego el estado del
        // usuario (que puede diferir si movió un slider) los sobrescribe.
        if (!v3Woken) v3Woken = DSPBridge.wakeNewPhysicsV3Organs()
        for ((i, st) in s.subBassStages.withIndex()) {
            DSPBridge.setNewPhysicsSubBassStage(
                i + 1, st.freqHz, st.q, st.gainDb, st.lowShelf, st.active
            )
        }
        DSPBridge.setNewPhysicsRoomTailMix(s.roomTailMix)
        DSPBridge.setNewPhysicsKlippelH3Mix(s.klippelH3Mix)
        // Ruta B (ABI v14, v2.5.472): el JNI solo escribe la Ruta A; sin
        // este empuje el efecto en audioserver nunca se entera (mismo
        // defecto que FusedControlsBridge.pushFusedToDaemon corrigió en la
        // 469 para los 23 campos fusionados). Independiente del JNI (si el
        // nativo falla, la Ruta B igual recibe los valores).
        pushToDaemon(s)
    }

    // ── Ruta B: Física Nueva al daemon por SET_PERCEPTUAL_STATE (ABI v14) ──
    /**
     * Mapeo Física Nueva → SET_PERCEPTUAL_STATE. Espejo del patrón
     * `FusedControlsBridge.buildFusedPerceptualFields` (misma acción, mismo
     * canal, mismo estilo: un Map<String, Float> puro, clampado, testeable
     * sin el socket). El daemon hace merge (ausente = se conserva el
     * vigente), así que este mensaje solo trae los 17 np_* y no toca
     * ningún campo fusionado ni legacy.
     *
     * CLAVES — DOBLE FORMA A PROPÓSITO (compatibilidad ABI v14):
     * cada campo viaja con DOS claves del MISMO valor clampado:
     *  - snake_case canónica (`np_amount`, `np_lgn_mix`, ...): idéntica
     *    al campo del snapshot `OmegaDspSnapshot.np_*`
     *    (omega_control_bus.h) y al atomic `g_np_*`; es la forma de la
     *    lista de la tarea ABI v14.
     *  - camelCase alias (`npAmount`, `npLgnMix`, ...): la convención
     *    histórica de las claves JSON de SET_PERCEPTUAL_STATE
     *    (`orbeAmount` ← `orbe_amount`, `holosGamma` ← `holos_gamma`,
     *    `hyperAmount` ← `hyper_amount`).
     * El lado daemon (responder + PendingPerceptual) se está añadiendo en
     * paralelo y no se pudo verificar en este checkout qué forma parsea;
     * el responder ignora las claves que no conoce (lee solo las que
     * busca por nombre), así que mandar ambas no tiene coste ni riesgo:
     * la que el daemon conozca se aplica y la otra se ignora. Cuando el
     * lado daemon quede confirmado, se puede borrar el alias que sobre.
     *
     * TIPOS: los 15 floats viajan como número JSON; los 2 enteros
     * (np_eq_class, np_abx_mode) viajan como Float (igual que
     * `hyperPreset`: `jni.toFloat()` → `toDouble()` en el JSON) y el
     * daemon los castea a int32 al publicar el snapshot.
     *
     * PUERTA DE BOCINAS: las claves np_* NO están en el mapa de
     * `RouteBDeviceGate.gatePerceptualCommandJson` (que solo reescribe
     * sus claves conocidas por regex), así que pasan intactas tanto en
     * audífonos como en bocinas/Genezi. No se gatean aquí a propósito:
     * np_amount es 0.0 por defecto (bypass) y el usuario lo enciende de
     * forma explícita; si Esse decide que en bocinas debe ir a 0, esa
     * puerta pertenece al lado daemon/efecto (o a una extensión del mapa
     * del gate), no a este empuje.
     *
     * CAMPOS SIN SLIDER EN LA UI (viajan con su valor persistido /
     * default, no se inventa nada): torsoEchoMix (0.18), azimuthDeg
     * (0.0), elevationDeg (0.0), boostCapDb (3.0), airCompMix (0.0).
     * abxOverrideMode SÍ tiene UI (botón ABX cíclico) pero NO se persiste
     * a propósito (sesión): cada arranque vuelve a 0 y así viaja al
     * daemon hasta que el usuario lo cambie.
     *
     * Devuelve el mapa campo → valor (puro, sin Android) para poder
     * unit-testear el mapeo sin el socket.
     */
    fun buildNewPhysicsPerceptualFields(s: NewPhysicsState): Map<String, Float> {
        val c = s.clamp()
        return linkedMapOf(
            "np_amount" to c.amount,
            "npAmount" to c.amount,
            "np_lgn_mix" to c.lgnPinnaMix,
            "npLgnMix" to c.lgnPinnaMix,
            "np_bs_damp" to c.brightSpotDamp,
            "npBsDamp" to c.brightSpotDamp,
            "np_torso_mix" to c.torsoEchoMix,
            "npTorsoMix" to c.torsoEchoMix,
            "np_head_radius_m" to c.headRadiusM,
            "npHeadRadiusM" to c.headRadiusM,
            "np_az_deg" to c.azimuthDeg,
            "npAzDeg" to c.azimuthDeg,
            "np_el_deg" to c.elevationDeg,
            "npElDeg" to c.elevationDeg,
            "np_eq_class" to c.eqClass.toFloat(),
            "npEqClass" to c.eqClass.toFloat(),
            "np_omega_sig_mix" to c.omegaSignatureMix,
            "npOmegaSigMix" to c.omegaSignatureMix,
            "np_boost_cap_db" to c.boostCapDb,
            "npBoostCapDb" to c.boostCapDb,
            "np_xtalk_mix" to c.crosstalkCompMix,
            "npXtalkMix" to c.crosstalkCompMix,
            "np_sigma_dist_m" to c.sigmaDistanceM,
            "npSigmaDistM" to c.sigmaDistanceM,
            "np_spl_db" to c.splEstimateDb,
            "npSplDb" to c.splEstimateDb,
            "np_air_mix" to c.airCompMix,
            "npAirMix" to c.airCompMix,
            "np_volterra_mix" to c.volterraMix,
            "npVolterraMix" to c.volterraMix,
            "np_free_energy_mix" to c.freeEnergyMix,
            "npFreeEnergyMix" to c.freeEnergyMix,
            "np_abx_mode" to c.abxOverrideMode.toFloat(),
            "npAbxMode" to c.abxOverrideMode.toFloat(),
        )
    }

    /**
     * Empuja los 17 controles de Física Nueva al daemon por
     * SET_PERCEPTUAL_STATE (mismo canal que los controles legacy y
     * fusionados). El daemon hace merge y publica un snapshot nuevo; el
     * efecto lo aplica vía omega_apply_snapshot. Fail-closed: si el
     * daemon no está o el envío falla, se loguea y no se rompe nada (la
     * Ruta A ya recibió los valores por JNI).
     */
    fun pushToDaemon(s: NewPhysicsState) {
        try {
            val fields = buildNewPhysicsPerceptualFields(s)
            val json = JSONObject().apply {
                put("action", "SET_PERCEPTUAL_STATE")
                for ((key, value) in fields) {
                    put(key, value.toDouble())
                }
            }
            val ok = OmegaEngineBridge.sendCommand(json)
            if (!ok) Log.w(TAG, "pushToDaemon: SET_PERCEPTUAL_STATE (np_*) no llegó al daemon")
        } catch (t: Throwable) {
            Log.e(TAG, "pushToDaemon (np_*) falló", t)
        }
    }

    /** Telemetría en vivo del motor Ruta A (8 floats; ceros si el nativo no está cargado). */
    fun readTelemetry(): NewPhysicsTelemetryUi =
        NewPhysicsTelemetryUi.fromArray(DSPBridge.getNewPhysicsTelemetry())

    /**
     * Aplica una calibración medida (salida de tools/genezi_calibrate.py):
     * tri21 L/R (21 floats) + H_b 24 bandas (dB) + headroom 24 bandas (dB).
     * Devuelve false si el kernel no pasa la puerta (Σ|k| ≤ 0.35, etc.).
     */
    fun applyCalibration(
        tri21L: FloatArray, tri21R: FloatArray,
        hb24: DoubleArray, headroom24: DoubleArray, fLo: Double, fHi: Double
    ): Boolean = DSPBridge.loadNewPhysicsCalibration(tri21L, tri21R, hb24, headroom24, fLo, fHi)

    /** Telemetría v3 en vivo del motor Ruta A (9 floats; defaults si el nativo no está cargado). */
    fun readV3Telemetry(): NewPhysicsV3TelemetryUi =
        NewPhysicsV3TelemetryUi.fromArray(DSPBridge.getNewPhysicsV3Telemetry())

    /**
     * Lista las 200 RIR del dataset (assets/ivanna_omega/rir/metadata.csv),
     * ordenadas por cercanía de su distancia a los 3.5 m de Esse al Genezi
     * (la primera es la más próxima; rir_0101.wav, 3.70 m, es el default).
     * Fail-closed: si el CSV no se lee, devuelve solo el default.
     */
    fun listRirs(ctx: Context): List<RirEntry> {
        val fallback = listOf(RirEntry("rir_0101.wav", 3.5, 2.9, 4.6, 3.70, 0.34))
        return runCatching {
            val text = ctx.assets.open("ivanna_omega/rir/metadata.csv")
                .bufferedReader().use { it.readText() }
            val entries = text.lineSequence().drop(1).mapNotNull { line ->
                val c = line.split(",")
                if (c.size < 12) return@mapNotNull null
                val file = c[0].trim()
                if (!file.matches(Regex("rir_\\d{4}\\.wav"))) return@mapNotNull null
                RirEntry(
                    file = file,
                    widthM = c[1].toDoubleOrNull() ?: return@mapNotNull null,
                    heightM = c[2].toDoubleOrNull() ?: return@mapNotNull null,
                    depthM = c[3].toDoubleOrNull() ?: return@mapNotNull null,
                    distanceM = c[10].toDoubleOrNull() ?: return@mapNotNull null,
                    rt60 = c[11].toDoubleOrNull() ?: return@mapNotNull null
                )
            }.toList()
            if (entries.isEmpty()) fallback
            else entries.sortedBy { kotlin.math.abs(it.distanceM - 3.5) }
        }.getOrDefault(fallback)
    }

    /**
     * Carga una RIR del dataset como cola de sala (Ruta A). Lee el WAV
     * estéreo PCM16 del asset, lo pasa a float y al JNI (que recorta
     * onset+25 ms y remuestrea a la sr del motor). Fail-closed: cualquier
     * anomalía (asset ausente, WAV no PCM16, muy corto) -> false y la
     * cola anterior sigue sonando.
     */
    fun loadRoomRir(ctx: Context, assetName: String, tailMix: Float): Boolean {
        val wav = runCatching { readRirWav(ctx, assetName) }.getOrNull() ?: return false
        return DSPBridge.loadNewPhysicsRoomTail(wav.first, wav.second, wav.third, tailMix)
    }

    // Parser WAV mínimo (PCM16, mono/estéreo, 8k..96k): camina los
    // subchunks (fmt / data / otros) sin asumir offsets fijos. El dataset
    // es estéreo 16 kHz/16-bit; mono se duplica a los dos canales.
    private fun readRirWav(ctx: Context, assetName: String): Triple<FloatArray, FloatArray, Int>? {
        val bytes = ctx.assets.open("ivanna_omega/rir/$assetName").use { it.readBytes() }
        if (bytes.size < 44) return null
        fun strAt(o: Int) = String(bytes, o, 4, Charsets.US_ASCII)
        if (strAt(0) != "RIFF" || strAt(8) != "WAVE") return null
        fun u16(o: Int) = (bytes[o].toInt() and 0xFF) or ((bytes[o + 1].toInt() and 0xFF) shl 8)
        fun u32(o: Int) = (bytes[o].toInt() and 0xFF) or ((bytes[o + 1].toInt() and 0xFF) shl 8) or
            ((bytes[o + 2].toInt() and 0xFF) shl 16) or ((bytes[o + 3].toInt() and 0xFF) shl 24)
        var channels = 0
        var rate = 0
        var bits = 0
        var dataOff = -1
        var dataLen = 0
        var o = 12
        while (o + 8 <= bytes.size) {
            val id = strAt(o)
            val size = u32(o + 4)
            val body = o + 8
            if (body + size > bytes.size) return null
            when (id) {
                "fmt " -> {
                    if (size < 16 || u16(body) != 1) return null  // solo PCM
                    channels = u16(body + 2)
                    rate = u32(body + 4)
                    bits = u16(body + 14)
                }
                "data" -> {
                    dataOff = body
                    dataLen = size
                }
            }
            o = body + size + (size and 1)  // chunks pares: byte de relleno
        }
        if (channels !in 1..2 || rate !in 8000..96000 || bits != 16 ||
            dataOff < 0 || dataLen < 3200
        ) return null
        val frames = dataLen / 2 / channels
        val l = FloatArray(frames)
        val r = FloatArray(frames)
        for (i in 0 until frames) {
            val base = dataOff + i * channels * 2
            fun sample(ch: Int): Float {
                val v = (bytes[base + ch * 2].toInt() and 0xFF) or
                    (bytes[base + ch * 2 + 1].toInt() shl 8)
                return v / 32768f
            }
            l[i] = sample(0)
            r[i] = sample(if (channels == 2) 1 else 0)
        }
        return Triple(l, r, rate)
    }
}
