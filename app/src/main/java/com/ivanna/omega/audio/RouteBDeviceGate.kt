package com.ivanna.omega.audio

import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.util.Log

/**
 * Puerta de Ruta B por dispositivo de salida (0032, v2.5.460).
 *
 * EL PROBLEMA (verificado en vivo por Esse + en código, 2026-11-16):
 * tras correr la APK, TODAS las apps suenan como filtro paso alto (sin
 * graves). El módulo SOLO suena bien. Mecanismo: PersistedStateRestorer
 * manda 3 SET_PERCEPTUAL_STATE al daemon del módulo al arrancar —
 * general (spatialWidth=1.55, harmonicGain=0.78, antiDolby=0.85,
 * compressor=-5.5), ORBE (orbeAmount=1.0 por defecto) y HOLOS
 * (holosAmount=1.0 por defecto, ATREVIDA 502) — y el daemon los aplica al efecto de
 * AudioFlinger (Ruta B), que procesa TODO el audio del teléfono. ORBE es
 * binaural (para audífonos) y el widener se come el medio donde viven los
 * graves: por bocinas eso adelgaza el sonido = paso alto. El estado
 * persiste en el daemon hasta reiniciar IVANNA y cerrar la app de música.
 * El fix 0023 solo puso la BASE del daemon en neutro; la APK volvía a
 * meter la coloración al arrancar.
 *
 * EL FIX: ORBE/HOLOS/widener conscientes del dispositivo de salida.
 * Audífonos → los montos del usuario pasan tal cual (ORBE y HOLOS siguen
 * ACTIVOS por defecto, directiva vigente 2026-11-08: esto es ENRUTADO
 * por dispositivo, NO apagado). Bocinas → neutro (orbeAmount=0,
 * holosAmount=0, spatialWidth=1.0, harmonicGain=0, antiDolby=0,
 * compressor=0, loudnessTarget=0, exciterRed=0).
 *
 * TERCER MODO — GENEZI (v2.5.483, hallazgo Esse 2026-11-20 05:18 CST):
 * el DAC USB JieLi → Genezi NO es bocina plana ni audífono: los montos
 * BINAURALES del usuario pasan (ORBE/HOLOS, spatialWidth, harmonicGain,
 * antiDolby) pero los GENERALES quedan en neutro (compressor=0,
 * loudnessTarget=0, exciterRed=0). El flip a HEADPHONES NO sirve: también
 * deja pasar compressor −5.5 y loudness −16 del restaurador (volumen
 * caído + opaco del hallazgo 0032). La protección 0032 contra eso DEBE
 * quedarse; lo que se quita es solo el apagado del holograma. Ver
 * RouteBOutput.GENEZI.
 *
 * EXTENSIÓN (hallazgo de Esse 2026-11-16 18:26 CST): Qobuz + módulo
 * suena BIEN; al prender la APK, INMEDIATAMENTE el volumen BAJA y el
 * sonido se vuelve OPACO (sordo), sin tronidos. La caída INMEDIATA
 * apunta al compresor (−5.5) y al loudness target (−16 LUFS): ORBE/
 * HOLOS/widener no bajan el volumen de golpe; el compresor sí aplasta
 * la ganancia y el loudness tira del nivel. Por eso el set neutro para
 * bocinas incluye TAMBIÉN compressor=0 (bypass; omega_apply_snapshot
 * lo salta con >= 0), loudnessTarget=0 (bypass; valor de makeNeutral)
 * y exciterRed=0 (makeNeutral). El highCut se queda en 19500 Hz (corte
 * suave, no es la causa del opaco; subirlo a 24000 queda como follow-up).
 *
 * SUTILEZA CRÍTICA — el Genezi de Esse va por adaptador DAC USB-C (JieLi):
 * dumpsys audio 2026-11-20 05:3x CST lo muestra como "USB Headset Out"
 * (AUDIO_DEVICE_OUT_USB_HEADSET, puerto 30, card 0), NO como USB_DEVICE:
 * el JieLi enumera como USB_HEADSET (a veces solo, a veces junto a
 * USB_DEVICE — la enumeración FLAPEA entre re-enumeraciones). Un
 * USB_HEADSET puro es ambiguo: puede ser el JieLi→Genezi o un audífono
 * USB real; el tipo NO los distingue. Por eso el selector usbDacTarget
 * decide para CUALQUIER endpoint de audio USB (USB_DEVICE, USB_ACCESSORY
 * y USB_HEADSET): SPEAKERS → Genezi, HEADPHONES → audífonos. Sin este
 * arreglo, el gate veía "audífonos" cuando el JieLi enumeraba solo como
 * USB_HEADSET (AntiDolby 0.71 de la IA llegaba vivo al daemon) y veía
 * "bocinas" cuando enumeraba como USB_DEVICE (ORBE→0): los valores
 * oscilaban entre escritores según la enumeración del momento. El
 * agujero era la rama "USB_HEADSET puro = audífono siempre" de la
 * sobrecarga de 2 args (hasUsbDac era false y la precedencia por tipos
 * lo resolvía sin consultar el selector).
 *
 * PRECEDENCIA (misma que OutputRoutePolicy.select: USB > BT > cableado >
 * bocina): `getDevices(GET_DEVICES_OUTPUTS)` enumera TODOS los endpoints
 * conectados, no la ruta activa de AudioPolicy. La clase ganadora decide;
 * dentro de cada clase, el tipo audífono gana al tipo bocina (USB_HEADSET
 * sobre USB_DEVICE, A2DP sobre BLE_SPEAKER, WIRED_HEADPHONES sobre
 * AUX_LINE). Caso mixto ambiguo (DAC USB + audífono BT conectados) → gana
 * USB → neutro: fail-closed hacia no colorear bocinas.
 *
 * FAIL-CLOSED: sin AudioManager, sin dispositivos, o si la lectura falla
 * → bocinas (neutro). Nunca se mandan montos binaurales sin un audífono
 * determinado.
 *
 * ESTE ARCHIVO: la decisión (`gate` / `isHeadphonesOutput` sobre tipos)
 * es PURA (sin Android) y se testea en host. Los adaptadores que leen el
 * AudioManager real (`isHeadphonesOutput(am)`, `gateForAudioManager`)
 * son finos y no se testean en host (sin SDK en el host; la CI los
 * compila).
 *
 * Lecciones 0030/459 aplicadas: NO se usa `AudioDescriptor` (no tiene
 * device/encodings/sampleRates) ni `IntArray?.orEmpty()` (no existe en la
 * stdlib); los tipos se leen de `AudioDeviceInfo.type` (API 23+) y el
 * neutro es un `val` top-level (los tests NO resuelven símbolos de
 * companion object).
 */

/**
 * Selector explícito de transductor para DAC USB (FASE 2, v2.5.462).
 *
 * Un descriptor USB no distingue audífonos de la entrada analógica de un
 * amplificador: el adaptador DAC JieLi reporta TYPE_USB_DEVICE tanto si
 * alimenta los Genezi como si alimenta unos audífonos. Sin este selector,
 * RouteBDeviceGate asume que todo DAC USB es bocina (0032) y apaga
 * ORBE/HOLOS aunque haya audífonos conectados al adaptador.
 *
 * SPEAKERS (default, fail-closed = comportamiento vigente): el DAC USB
 * alimenta altavoces (Genezi). HEADPHONES: el DAC USB alimenta audífonos.
 * Persistido en SpatialAudioPrefs.usbDacTarget. Solo decide cuando hay
 * un DAC USB presente; sin DAC, la precedencia por tipos manda.
 */
enum class UsbDacTarget { SPEAKERS, HEADPHONES }

/**
 * Salida efectiva de Ruta B (v2.5.483, tercer modo).
 *
 * HEADPHONES: audífonos determinados (BT, cableado, o DAC USB con
 * selector HEADPHONES) → pasan binaural Y general del usuario.
 * GENEZI: DAC USB con selector SPEAKERS (la cadena JieLi → Genezi) →
 * pasan los montos BINAURALES del usuario (ORBE/HOLOS, spatialWidth,
 * harmonicGain, antiDolby) y los GENERALES van a neutro (compressor=0,
 * loudnessTarget=0, exciterRed=0 — protección 0032 intacta).
 * SPEAKERS: bocina plana/genérica o indeterminado (fail-closed) →
 * todo neutro (comportamiento 0032 vigente).
 */
enum class RouteBOutput { HEADPHONES, GENEZI, SPEAKERS }

/** Montos binaurales que la APK manda al daemon (Ruta B). */
data class RouteBBinauralAmounts(
    val orbeAmount: Float,
    val holosAmount: Float,
    val spatialWidth: Float,
    val harmonicGain: Float,
    val antiDolby: Float
)

// Top-level (NO en companion object): los tests unitarios no resuelven
// símbolos de companion object (lección 0030/459). Neutro = identidad
// acústica para Ruta B: sin binaural, sin widener, sin coloración.
val ROUTE_B_NEUTRAL_AMOUNTS = RouteBBinauralAmounts(
    orbeAmount = 0f,
    holosAmount = 0f,
    spatialWidth = 1.0f,
    harmonicGain = 0f,
    antiDolby = 0f
)

/** Montos generales (no binaurales) que la APK manda al daemon (Ruta B). */
data class RouteBGeneralAmounts(
    val compressor: Float,
    val exciterRed: Float,
    val loudnessTarget: Float
)

// Neutro general para bocinas (extensión 0032, hallazgo Esse 18:26 CST):
// compressor 0 = bypass (omega_apply_snapshot lo salta con >= 0);
// loudnessTarget 0 = bypass (valor de makeNeutral); exciterRed 0 =
// makeNeutral. Sin ellos, el volumen caía al prender la APK aunque
// ORBE/HOLOS/widener ya fueran neutros.
val ROUTE_B_NEUTRAL_GENERAL_AMOUNTS = RouteBGeneralAmounts(
    compressor = 0f,
    exciterRed = 0f,
    loudnessTarget = 0f
)

object RouteBDeviceGate {
    private const val TAG = "IVANNA.RouteBDeviceGate"

    // Tipos que son AUDÍFONOS (binaural OK): los montos del usuario pasan.
    // TYPE_BLE_HEADSET es API 31+ (constante inlined, compila con
    // compileSdk 35; en API < 31 simplemente nunca aparece en getDevices()).
    // TYPE_BLE_HEARING_AID es API 36 (Android 16) y NO existe en
    // compileSdk 35 — por eso se usa el valor literal 33, verificado en
    // developer.android.com (AudioDeviceInfo, Constant Value: 33).
    // En API < 36 el tipo nunca aparece, así que el literal es seguro.
    private const val TYPE_BLE_HEARING_AID = 33
    val HEADPHONE_TYPES = intArrayOf(
        AudioDeviceInfo.TYPE_WIRED_HEADSET,     // 3: cable con mic
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,  // 4: cable sin mic
        AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,    // 8: BT clásico
        AudioDeviceInfo.TYPE_USB_HEADSET,       // 22: USB con mic
        AudioDeviceInfo.TYPE_HEARING_AID,       // 23: audífono
        AudioDeviceInfo.TYPE_BLE_HEADSET,       // 26: BLE headset
        TYPE_BLE_HEARING_AID                    // 33: BLE hearing aid (API 36)
    )

    fun isHeadphoneType(type: Int): Boolean = type in HEADPHONE_TYPES

    /**
     * Decisión pura: ¿la salida efectiva son audífonos?
     *
     * Precedencia por clase (USB > BT > cableado > bocina), igual que
     * OutputRoutePolicy.select. Dentro de cada clase, el tipo audífono
     * gana al tipo bocina. Sin dispositivos, o si la clase ganadora es de
     * bocinas → false (neutro, fail-closed).
     */
    fun isHeadphonesOutput(deviceTypes: IntArray): Boolean {
        if (deviceTypes.isEmpty()) return false
        // USB: un DAC (USB_DEVICE) o accesorio gana a un BT conectado;
        // solo USB_HEADSET es audífono en esta clase.
        val hasUsb = deviceTypes.any {
            it == AudioDeviceInfo.TYPE_USB_HEADSET ||
                it == AudioDeviceInfo.TYPE_USB_DEVICE ||
                it == AudioDeviceInfo.TYPE_USB_ACCESSORY
        }
        if (hasUsb) return deviceTypes.any { it == AudioDeviceInfo.TYPE_USB_HEADSET }
        // BT: A2DP / BLE headset / audífono son audífonos; BLE_SPEAKER es
        // bocina. TYPE_HEARING_AID (clásico, API 23+) va en esta clase:
        // un audífono se conecta por BT; sin esto, un audífono solo caía
        // al `return false` final (cazado por el puerto Python de la
        // tabla de decisión antes de pushear).
        val hasBt = deviceTypes.any {
            it == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
                it == AudioDeviceInfo.TYPE_BLE_HEADSET ||
                it == AudioDeviceInfo.TYPE_BLE_SPEAKER ||
                it == TYPE_BLE_HEARING_AID ||
                it == AudioDeviceInfo.TYPE_HEARING_AID
        }
        if (hasBt) {
            return deviceTypes.any {
                it == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
                    it == AudioDeviceInfo.TYPE_BLE_HEADSET ||
                    it == TYPE_BLE_HEARING_AID ||
                    it == AudioDeviceInfo.TYPE_HEARING_AID
            }
        }
        // Cableado: headset/headphones son audífonos; AUX_LINE / LINE_ANALOG
        // son salida de línea (al Genezi o a bocinas) → bocinas.
        val hasWired = deviceTypes.any {
            it == AudioDeviceInfo.TYPE_WIRED_HEADSET ||
                it == AudioDeviceInfo.TYPE_WIRED_HEADPHONES ||
                it == AudioDeviceInfo.TYPE_AUX_LINE ||
                it == AudioDeviceInfo.TYPE_LINE_ANALOG
        }
        if (hasWired) {
            return deviceTypes.any {
                it == AudioDeviceInfo.TYPE_WIRED_HEADSET ||
                    it == AudioDeviceInfo.TYPE_WIRED_HEADPHONES
            }
        }
        // Bocina integrada, HDMI, resto, desconocido → bocinas (neutro).
        return false
    }

    /**
     * ¿Hay un endpoint de audio USB conectado?
     * Puro. Incluye TYPE_USB_HEADSET (v2.5.483): el adaptador JieLi
     * enumera como USB_HEADSET (dumpsys audio 2026-11-20: "USB Headset
     * Out", puerto 30) aunque alimente el Genezi, así que un USB_HEADSET
     * es ambiguo y NO es "inequívocamente un auricular" — el selector
     * usbDacTarget decide (ver isHeadphonesOutput de 2 args). Un
     * audífono USB real con el selector en SPEAKERS (default) resuelve
     * a GENEZI (binaural pasa, general neutro); para la cadena completa
     * de audífonos debe poner el selector en HEADPHONES (override
     * manual en UI, follow-up pendiente desde FASE 2).
     */
    fun hasUsbDac(deviceTypes: IntArray): Boolean = deviceTypes.any {
        it == AudioDeviceInfo.TYPE_USB_DEVICE ||
            it == AudioDeviceInfo.TYPE_USB_ACCESSORY ||
            it == AudioDeviceInfo.TYPE_USB_HEADSET
    }

    /**
     * Decisión pura con selector de transductor (FASE 2, corregida
     * v2.5.483).
     *
     * Con endpoint USB presente (USB_DEVICE / USB_ACCESSORY /
     * USB_HEADSET), el selector del usuario decide: HEADPHONES →
     * audífonos (pasan los montos binaurales), SPEAKERS → bocinas/
     * Genezi (tercer modo). Antes de la 483, un USB_HEADSET puro (sin
     * USB_DEVICE/USB_ACCESSORY) NO llegaba a esta rama (hasUsbDac era
     * false) y la precedencia por tipos lo resolvía como audífono
     * siempre, ignorando el selector — el agujero por el que el JieLi
     * (que enumera como usb_headset) pasaba AntiDolby 0.71 de la IA y
     * dejaba ORBE a merced de la enumeración del momento. Sin DAC USB,
     * delega en la precedencia por tipos existente.
     */
    fun isHeadphonesOutput(deviceTypes: IntArray, usbDacTarget: UsbDacTarget): Boolean {
        if (deviceTypes.isEmpty()) return false
        if (hasUsbDac(deviceTypes)) {
            return usbDacTarget == UsbDacTarget.HEADPHONES
        }
        return isHeadphonesOutput(deviceTypes)
    }

    /**
     * Salida efectiva de Ruta B en tres estados (v2.5.483). Pura.
     * HEADPHONES si la decisión con selector dice audífonos; GENEZI si
     * hay endpoint USB y el selector dice SPEAKERS (la cadena del
     * Genezi); SPEAKERS en el resto (bocina plana/genérica o sin
     * dispositivo determinable — fail-closed a neutro total).
     */
    fun routeBOutput(deviceTypes: IntArray, usbDacTarget: UsbDacTarget): RouteBOutput {
        if (isHeadphonesOutput(deviceTypes, usbDacTarget)) return RouteBOutput.HEADPHONES
        if (hasUsbDac(deviceTypes) && usbDacTarget == UsbDacTarget.SPEAKERS) return RouteBOutput.GENEZI
        return RouteBOutput.SPEAKERS
    }

    /** Adaptador: salida efectiva en tres estados desde el AudioManager real. */
    fun routeBOutput(am: AudioManager, usbDacTarget: UsbDacTarget): RouteBOutput {
        val types = runCatching { am.getDevices(AudioManager.GET_DEVICES_OUTPUTS) }
            .getOrNull()?.map { it.type }?.toIntArray() ?: return RouteBOutput.SPEAKERS
        val output = routeBOutput(types, usbDacTarget)
        Log.i(TAG, "Salida Ruta B: $output (tipos=${types.joinToString()}, usbDac=$usbDacTarget)")
        return output
    }

    /** Montos efectivos para Ruta B: usuario en audífonos, neutro en bocinas. */
    fun gate(user: RouteBBinauralAmounts, deviceTypes: IntArray): RouteBBinauralAmounts =
        gate(user, deviceTypes, UsbDacTarget.SPEAKERS)

    /** Montos efectivos con selector de transductor (FASE 2). */
    fun gate(
        user: RouteBBinauralAmounts,
        deviceTypes: IntArray,
        usbDacTarget: UsbDacTarget
    ): RouteBBinauralAmounts =
        gate(user, routeBOutput(deviceTypes, usbDacTarget))

    /**
     * Montos binaurales efectivos en tres estados (v2.5.483): en GENEZI
     * pasan los del usuario (ORBE/HOLOS del Genezi); solo SPEAKERS va a
     * neutro. HEADPHONES pasa intacto (comportamiento vigente).
     */
    fun gate(user: RouteBBinauralAmounts, output: RouteBOutput): RouteBBinauralAmounts =
        if (output == RouteBOutput.SPEAKERS) ROUTE_B_NEUTRAL_AMOUNTS else user

    /** Montos generales efectivos: usuario en audífonos, neutro en bocinas. */
    fun gateGeneral(user: RouteBGeneralAmounts, deviceTypes: IntArray): RouteBGeneralAmounts =
        gateGeneral(user, deviceTypes, UsbDacTarget.SPEAKERS)

    /** Montos generales efectivos con selector de transductor (FASE 2). */
    fun gateGeneral(
        user: RouteBGeneralAmounts,
        deviceTypes: IntArray,
        usbDacTarget: UsbDacTarget
    ): RouteBGeneralAmounts =
        gateGeneral(user, routeBOutput(deviceTypes, usbDacTarget))

    /**
     * Montos generales efectivos en tres estados (v2.5.483): SOLO
     * HEADPHONES pasa los del usuario; GENEZI y SPEAKERS van a neutro
     * (protección 0032: compressor −5.5 / loudness −16 nunca llegan al
     * Genezi aunque el holograma sí pase).
     */
    fun gateGeneral(user: RouteBGeneralAmounts, output: RouteBOutput): RouteBGeneralAmounts =
        if (output == RouteBOutput.HEADPHONES) user else ROUTE_B_NEUTRAL_GENERAL_AMOUNTS

    /** Montos generales efectivos desde el booleano ya resuelto (PersistedStateRestorer). */
    fun gateGeneral(user: RouteBGeneralAmounts, headphones: Boolean): RouteBGeneralAmounts =
        if (headphones) user else ROUTE_B_NEUTRAL_GENERAL_AMOUNTS

    /** Conveniencia: mismos tipos, desde los AudioDeviceInfo del SO. */
    fun gate(user: RouteBBinauralAmounts, devices: Array<out AudioDeviceInfo>): RouteBBinauralAmounts =
        gate(user, devices.map { it.type }.toIntArray())

    /**
     * Adaptador: lee los dispositivos de salida del AudioManager real.
     * Nunca lanza; sin AudioManager, sin dispositivos o si la lectura
     * falla → false (bocinas, neutro).
     */
    fun isHeadphonesOutput(am: AudioManager): Boolean =
        isHeadphonesOutput(am, UsbDacTarget.SPEAKERS)

    /** Adaptador con selector de transductor (FASE 2). */
    fun isHeadphonesOutput(am: AudioManager, usbDacTarget: UsbDacTarget): Boolean {
        val types = runCatching { am.getDevices(AudioManager.GET_DEVICES_OUTPUTS) }
            .getOrNull()?.map { it.type }?.toIntArray() ?: return false
        val headphones = isHeadphonesOutput(types, usbDacTarget)
        Log.i(TAG, "Salida Ruta B: ${if (headphones) "audífonos" else "bocinas/neutro"} " +
            "(tipos=${types.joinToString()}, usbDac=$usbDacTarget)")
        return headphones
    }

    /** Adaptador: montos efectivos desde el AudioManager real. */
    fun gateForAudioManager(am: AudioManager, user: RouteBBinauralAmounts): RouteBBinauralAmounts =
        if (isHeadphonesOutput(am)) user else ROUTE_B_NEUTRAL_AMOUNTS

    /** Adaptador en tres estados (v2.5.483): montos efectivos desde el AudioManager real + selector. */
    fun gateForAudioManager(am: AudioManager, user: RouteBBinauralAmounts, usbDacTarget: UsbDacTarget): RouteBBinauralAmounts =
        gate(user, routeBOutput(am, usbDacTarget))

    /**
     * Helper puro (0037): neutraliza los campos perceptuales de un comando
     * JSON cuando la salida no son audífonos. Si headphones, el comando
     * vuelve intacto; si no, y SOLO si action es SET_PERCEPTUAL_STATE o
     * SEND_PERCEPTUAL_STATE, reescribe por regex los campos presentes a
     * neutro. Campos ausentes NO se añaden; comandos no perceptuales,
     * highCut y params secundarios quedan intactos. Los campos pueden ir
     * en el nivel superior o anidados en "state" (el regex sobre el
     * string entero cubre ambos).
     */
    fun gatePerceptualCommandJson(command: String, headphones: Boolean): String =
        gatePerceptualCommandJson(
            command,
            if (headphones) RouteBOutput.HEADPHONES else RouteBOutput.SPEAKERS
        )

    /**
     * Gate JSON en tres estados (v2.5.483). HEADPHONES: intacto.
     * SPEAKERS: neutro total (lista vigente 0037+0038+0040). GENEZI:
     * SOLO se neutralizan los campos generales (compressor,
     * loudnessTarget, exciterRed/exciterReduction), upmixing y holosEq_*;
     * orbeAmount, holosAmount, spatialWidth, harmonicGain y antiDolby
     * PASAN con los valores del usuario (el holograma del Genezi).
     *
     * Sello de modo (evol-holos, 2026-11-26): el "outputMode" que viaja
     * al daemon sigue al DISPOSITIVO resuelto (1 audífonos / 2 bocina
     * plana / 3 Genezi), no al spatial.mode guardado del push que lo
     * emite. Sin esto, el push fusionado —último escritor en restore()—
     * con spatial.mode=SPEAKERS devolvía el daemon a SPEAKERS aunque el
     * dispositivo fuera el Genezi, y el efecto apagaba el holograma
     * hasta el próximo pushRouteBState. Campo ausente NO se añade (como
     * el resto de campos: missingFieldsNotAdded).
     */
    fun gatePerceptualCommandJson(command: String, output: RouteBOutput): String {
        if (!command.contains("\"action\"")) return command
        if (!command.contains("SET_PERCEPTUAL_STATE") && !command.contains("SEND_PERCEPTUAL_STATE")) return command
        val modeStamp = when (output) {
            RouteBOutput.HEADPHONES -> "1"
            RouteBOutput.GENEZI -> "3"
            RouteBOutput.SPEAKERS -> "2"
        }
        val gated = OUTPUT_MODE_REGEX.replace(command) { "\"outputMode\":$modeStamp" }
        if (output == RouteBOutput.HEADPHONES) return gated
        val neutralMap = if (output == RouteBOutput.GENEZI) mapOf(
            "compressor" to "0",
            "loudnessTarget" to "0",
            "exciterRed" to "0",
            "exciterReduction" to "0",
            "upmixingEnabled" to "0",
            "upmixingImmersivity" to "0",
            // 0038+0040 Parche D: holosEq_* llegaban sin gate al daemon y al
            // snapshot (holosEqLow +12 / holosEqMid +4.13 medidos en vivo).
            // En GENEZI también van a neutro: la lista de pase es solo
            // orbe/holos/spatialWidth/harmonicGain/antiDolby.
            "holosEqLow" to "0",
            "holosEqMid" to "0",
            "holosEqHigh" to "0",
            "holosEqPresence" to "0",
            "holosEqTiltDb" to "0"
        ) else mapOf(
            "orbeAmount" to "0",
            "holosAmount" to "0",
            "spatialWidth" to "1.0",
            "harmonicGain" to "0",
            "antiDolby" to "0",
            "compressor" to "0",
            "loudnessTarget" to "0",
            "exciterRed" to "0",
            "exciterReduction" to "0",
            "upmixingEnabled" to "0",
            "upmixingImmersivity" to "0",
            // 0038+0040 Parche D: holosEq_* llegaban sin gate al daemon y al
            // snapshot (holosEqLow +12 / holosEqMid +4.13 medidos en vivo).
            "holosEqLow" to "0",
            "holosEqMid" to "0",
            "holosEqHigh" to "0",
            "holosEqPresence" to "0",
            "holosEqTiltDb" to "0"
        )
        val regex = Regex("\"(${neutralMap.keys.joinToString("|")})\"\\s*:\\s*-?\\d+(\\.\\d+)?([eE][+-]?\\d+)?")
        return regex.replace(gated) { matchResult ->
            val key = matchResult.groupValues[1]
            "\"$key\":${neutralMap.getValue(key)}"
        }
    }
}

private val OUTPUT_MODE_REGEX =
    Regex("\"outputMode\"\\s*:\\s*-?\\d+(\\.\\d+)?([eE][+-]?\\d+)?")
