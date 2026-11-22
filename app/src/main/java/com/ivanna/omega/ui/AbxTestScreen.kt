package com.ivanna.omega.ui

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.ivanna.omega.spatial.AbxTrialSession
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.spatial.AbxResultStore
import com.ivanna.omega.spatial.AbxStats
import com.ivanna.omega.spatial.IvannaSpatialManager
import com.ivanna.omega.audio.AudioEngine
import com.ivanna.omega.audio.FusedControlsBridge
import com.ivanna.omega.audio.MasterState
import com.ivanna.omega.audio.MasterStateController
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.spatial.LufsMatch
import com.ivanna.omega.spatial.MasterStateAbxSession
import com.ivanna.omega.spatial.OrbeHolosAbx
import com.ivanna.omega.spatial.OrbeHolosAbxSession
import com.ivanna.omega.spatial.OrbeHolosCondition
import org.json.JSONObject
import java.util.Date

@Composable
fun AbxTestScreen(onBack: () -> Unit) {
    // FASE 5: dos protocolos ABX en la misma pantalla. "HRTF" es el
    // existente (estático vs dinámico); "MASTER" es el nuevo (BYPASS /
    // LIMPIO_OFF / ACTIVO, matched-LUFS ±0.05). El modo MASTER delega
    // en MasterStateAbxScreen (abajo) y NO toca la lógica HRTF.
    // v2.5.471 (Repair 6 / F1): "ORBE_HOLOS" es el ABX ORBE·Ω+HOLOS·Ω
    // ON vs OFF, 16 ensayos, level-matched ±0.2 LU; delega en
    // OrbeHolosAbxScreen (abajo) y NO toca los otros dos protocolos.
    var abxMode by remember { mutableStateOf("HRTF") } // "HRTF" | "MASTER" | "ORBE_HOLOS"
    if (abxMode == "MASTER") {
        MasterStateAbxScreen(onBack = onBack, onBackToHrtf = { abxMode = "HRTF" })
        return
    }
    if (abxMode == "ORBE_HOLOS") {
        OrbeHolosAbxScreen(onBack = onBack, onBackToHrtf = { abxMode = "HRTF" })
        return
    }
    val context = androidx.compose.ui.platform.LocalContext.current
    var totalTrials by remember { mutableStateOf(AbxResultStore.validTrials(context).size) }
    var totalHits by remember { mutableStateOf(AbxResultStore.validTrials(context).count { it.optBoolean("correct") }) }
    var lastExport by remember { mutableStateOf<String?>(null) }
    var testActive by remember { mutableStateOf(false) }
    var currentSample by remember { mutableStateOf("A") } // A, B, X
    var selectedMatch by remember { mutableStateOf<String?>(null) }
    var scoreSpatial by remember { mutableStateOf(5f) }
    var scoreNatural by remember { mutableStateOf(5f) }
    
    val scenarioA = 1
    val scenarioB = 2
    val session = remember { AbxTrialSession() }
    val scope = rememberCoroutineScope()
    var originalTracking by remember { mutableStateOf<Boolean?>(null) }
    var trialSubject by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var readyToSubmit by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("Requiere HRTF cargado y seguimiento de cabeza disponible.") }

    fun restore() {
        originalTracking?.let { enabled ->
            if (enabled) IvannaSpatialManager.resumeHeadTracking()
            else IvannaSpatialManager.pauseHeadTracking()
        }
        originalTracking = null
    }
    DisposableEffect(Unit) { onDispose { restore(); session.cancel() } }

    fun listen(sample: String) {
        if (busy) return
        busy = true
        readyToSubmit = false
        scope.launch {
            try {
                check(IvannaNativeLib.isLoaded && IvannaSpatialManager.isHrtfConfirmed() &&
                    IvannaSpatialManager.activeSubject == trialSubject)
                val dynamic = session.scenario(sample) == 2
                if (dynamic) IvannaSpatialManager.resumeHeadTracking()
                else IvannaSpatialManager.pauseHeadTracking()
                // Registration alone is not evidence that a fresh sensor sample reached JNI.
                if (dynamic) {
                    repeat(20) {
                        if (!IvannaSpatialManager.isHeadTrackingFresh()) delay(50)
                    }
                }
                val accepted = IvannaSpatialManager.isHrtfConfirmed() &&
                    if (dynamic) IvannaSpatialManager.isHeadTrackingFresh()
                    else !IvannaSpatialManager.isHeadTrackingRegistered()
                session.confirm(sample, accepted)
                check(accepted)
                currentSample = sample
                readyToSubmit = session.canSubmit("A")
                status = "Muestra $sample preparada. Escucha antes de responder."
            } catch (_: Exception) {
                session.cancel(); restore(); testActive = false
                status = "Ensayo cancelado: no se confirmó el renderer/sensor. No se registraron resultados."
            } finally { busy = false }
        }
    }


    Column(
        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text("Prueba ABX - Validación Perceptual Espacial", style = MaterialTheme.typography.titleLarge)
        Text(status, style = MaterialTheme.typography.bodySmall)
        // FASE 5: entrada al ABX del estado maestro (BYPASS / LIMPIO_OFF /
        // ACTIVO, matched-LUFS). No cambia el protocolo HRTF de abajo.
        if (!testActive) {
            Spacer(modifier = Modifier.height(8.dp))
            Button(onClick = { abxMode = "MASTER" }) {
                Text("ABX Estado maestro — BYPASS / LIMPIO / ACTIVO (Fase 5)")
            }
            // v2.5.471 (Repair 6 / F1): ABX ORBE·Ω+HOLOS·Ω ON vs OFF,
            // 16 ensayos, level-matched ±0.2 LU. El dueño corre 2 sesiones.
            Spacer(modifier = Modifier.height(8.dp))
            Button(onClick = { abxMode = "ORBE_HOLOS" }) {
                Text("ABX ORBE·Ω+HOLOS·Ω — ON vs OFF, 16 ensayos (±0.2 LU)")
            }
        }
        Spacer(modifier = Modifier.height(32.dp))

        if (!testActive) {
            Button(enabled = !busy, onClick = {
                if (IvannaSpatialManager.isHrtfConfirmed()) {
                    originalTracking = IvannaSpatialManager.isHeadTrackingRegistered()
                    trialSubject = IvannaSpatialManager.activeSubject
                    session.begin(); testActive = true; selectedMatch = null
                    scoreSpatial = 5f; scoreNatural = 5f
                    listen("A")
                } else status = "Carga un HRTF en el motor espacial antes de iniciar."
            }) {
                Text("Iniciar Test Ciego")
            }
        } else {
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Button(enabled = !busy, onClick = { listen("A") }, colors = ButtonDefaults.buttonColors(containerColor = if (currentSample == "A") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) {
                    Text("Escuchar A")
                }
                Button(enabled = !busy, onClick = { listen("B") }, colors = ButtonDefaults.buttonColors(containerColor = if (currentSample == "B") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) {
                    Text("Escuchar B")
                }
                Button(enabled = !busy, onClick = { listen("X") }, colors = ButtonDefaults.buttonColors(containerColor = if (currentSample == "X") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) {
                    Text("Escuchar X")
                }
            }

            Spacer(modifier = Modifier.height(32.dp))
            Text("X es igual a:")
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Button(onClick = { selectedMatch = "A" }, colors = ButtonDefaults.buttonColors(containerColor = if (selectedMatch == "A") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) { Text("Muestra A") }
                Button(onClick = { selectedMatch = "B" }, colors = ButtonDefaults.buttonColors(containerColor = if (selectedMatch == "B") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) { Text("Muestra B") }
            }

            Spacer(modifier = Modifier.height(24.dp))
            Text("Localización y Externalización (1-10): ${scoreSpatial.toInt()}")
            Slider(value = scoreSpatial, onValueChange = { scoreSpatial = it }, valueRange = 1f..10f, steps = 8)
            
            Text("Naturalidad y Fatiga (1-10): ${scoreNatural.toInt()}")
            Slider(value = scoreNatural, onValueChange = { scoreNatural = it }, valueRange = 1f..10f, steps = 8)

            Spacer(modifier = Modifier.height(32.dp))
            Button(
                onClick = submit@{
                    val dynamic = session.scenario(currentSample) == 2
                    val stillReady = IvannaSpatialManager.isHrtfConfirmed() &&
                        IvannaSpatialManager.activeSubject == trialSubject &&
                        if (dynamic) IvannaSpatialManager.isHeadTrackingFresh()
                        else !IvannaSpatialManager.isHeadTrackingRegistered()
                    if (!stillReady) {
                        session.cancel(); restore(); testActive = false; readyToSubmit = false
                        status = "Ensayo cancelado: cambió el renderer o el sensor antes de registrar."
                        return@submit
                    }
                    val scenarioX = session.scenarioX
                    val correct = session.finish(requireNotNull(selectedMatch))
                    val result = JSONObject().apply {
                        put("timestamp", Date().time)
                        put("protocol", "ABX_Static_vs_DynamicHRTF")
                        put("protocolVersion", 2)
                        put("valid", true)
                        put("hrtfSubject", IvannaSpatialManager.activeSubject)
                        put("verification", "renderer-loaded-and-tracker-state; acoustic-path-not-instrumented")
                        put("scenarioA", scenarioA)
                        put("scenarioB", scenarioB)
                        put("scenarioX", scenarioX)
                        put("userSelection", selectedMatch)
                        put("correct", correct)
                        put("scoreSpatial", scoreSpatial)
                        put("scoreNatural", scoreNatural)
                    }
                    AbxResultStore.record(context, result)
                    totalTrials += 1
                    if (correct) totalHits += 1
                    
                    restore()
                    testActive = false
                    readyToSubmit = false
                    selectedMatch = null
                },
                enabled = !busy && readyToSubmit && session.canSubmit(selectedMatch)
            ) {
                Text("Enviar y Registrar Métricas")
            }
        }
        
        Spacer(modifier = Modifier.height(24.dp))
        if (totalTrials > 0) {
            val p = AbxStats.binomialTest(totalHits, totalTrials)
            val (ciLo, ciHi) = AbxStats.wilsonInterval95(totalHits, totalTrials)
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(modifier = Modifier.padding(16.dp)) {
                    Text("Ensayos válidos del protocolo v2", style = MaterialTheme.typography.titleMedium)
                    Text("Ensayos: $totalTrials · Aciertos: $totalHits " +
                         "(${if (totalTrials>0) (100.0*totalHits/totalTrials).toInt() else 0}%)")
                    Text("p-valor (binomial exacto, H0=0.5): ${"%.4f".format(p)}")
                    Text("IC 95%: [${"%.2f".format(ciLo)}, ${"%.2f".format(ciHi)}]")
                    Text(
                        if (totalTrials >= 10 && p < 0.05) "Diferenciación reportada (p<0.05); no certifica calidad ni la ruta acústica"
                        else "VEREDICTO: NO DEMOSTRADO",
                        color = if (totalTrials >= 10 && p < 0.05)
                            MaterialTheme.colorScheme.primary
                        else MaterialTheme.colorScheme.error
                    )
                    Spacer(modifier = Modifier.height(8.dp))
                    Button(onClick = {
                        lastExport = AbxResultStore.exportJson(context)?.absolutePath
                    }) { Text("Exportar JSON") }
                    lastExport?.let { Text("→ $it", style = MaterialTheme.typography.bodySmall) }
                }
            }
        }
        Button(onClick = { restore(); session.cancel(); onBack() }) {
            Text("Volver")
        }
    }
}

// ===========================================================================
// MasterStateAbxScreen — FASE 5: ABX ciego BYPASS / LIMPIO_OFF / ACTIVO.
//
// Protocolo "ABX_MasterState_Bypass_vs_Limpio_vs_Activo" (v1). Tres pares
// por comparación, orden barajado (MasterStateAbxSession). El oyente no
// sabe cuál es cuál: A y B son los dos estados del par, X es uno de ellos
// al azar; responde "X es igual a A o a B".
//
// MATCHED-LUFS (Regla de Oro): ningún par empieza sin igualar sus dos
// estados en LUFS a ±0.05 LU. La pantalla mide el LUFS de cada estado
// (mismo pasaje de referencia, 3 s por estado, promedio en dominio
// lineal) y aplica el trim con el master gain global (AudioEngine.
// setMasterGain, un escalar a la salida — no cambia el procesado, solo
// el nivel). Un paso de refinado si el residual supera ±0.05. El gate
// es fail-closed: beginPair() no arranca sin setLufsMatched(pair, true).
//
// AudioEngine SIN initialize(): libLoaded es companion (compartido con
// el motor vivo); getLufs() lee el medidor LUFS global vivo y
// setMasterGain() escribe el master gain global. NO llamar a
// initialize() aquí: re-correría nativeInit sobre el motor vivo.
//
// Al salir se restaura el estado previo al ABX y el master gain a 0 dB
// (línea base neutra). Validación en dispositivo pendiente: Kotlin no
// compila en host (sin SDK); verificar en el G85 que la pantalla abre,
// que el igualado LUFS llega a ±0.05 y que los 3 pares registran.
// ===========================================================================
@Composable
fun MasterStateAbxScreen(onBack: () -> Unit, onBackToHrtf: () -> Unit) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val scope = rememberCoroutineScope()
    val session = remember { MasterStateAbxSession() }
    val audioEngine = remember { AudioEngine() }
    var status by remember { mutableStateOf("Reproduce el mismo pasaje de referencia y pulsa «Igualar LUFS».") }
    var busy by remember { mutableStateOf(false) }
    var phase by remember { mutableStateOf("match") } // match | abx | done
    var currentPair by remember { mutableStateOf(session.currentPair) }
    var currentSample by remember { mutableStateOf("A") }
    var selectedMatch by remember { mutableStateOf<String?>(null) }
    var readyToSubmit by remember { mutableStateOf(false) }
    var lufsText by remember { mutableStateOf("") }
    var lastExport by remember { mutableStateOf<String?>(null) }
    var trims by remember { mutableStateOf<Map<MasterState, Double>>(emptyMap()) }
    var preAbxState by remember { mutableStateOf<MasterState?>(null) }

    fun restore() {
        preAbxState?.let { MasterStateController.submitState(it, "abx-exit") }
        runCatching { audioEngine.setMasterGain(0.0f) }
        preAbxState = null
    }
    DisposableEffect(Unit) { onDispose { restore() } }

    // Mide el LUFS (promedio en dominio lineal) de un estado durante
    // `seconds`. Con resetGain=true mide SIN trim (línea base); con
    // false mide CON el trim ya aplicado (verificación del residual).
    suspend fun measureLufs(state: MasterState, seconds: Double, resetGain: Boolean): Double {
        MasterStateController.submitState(state, "abx-lufs")
        if (resetGain) runCatching { audioEngine.setMasterGain(0.0f) }
        delay(500)
        val samples = mutableListOf<Double>()
        repeat((seconds * 4).toInt()) {
            val v = audioEngine.getLufs().toDouble()
            if (v.isFinite() && v > -70.0) samples.add(v)
            delay(250)
        }
        if (samples.isEmpty()) return Double.NaN
        val lin = samples.map { Math.pow(10.0, it / 10.0) }.average()
        return 10.0 * Math.log10(lin)
    }

    fun runLufsMatch(pair: MasterStateAbxSession.Pair) {
        if (busy) return
        busy = true
        if (preAbxState == null) preAbxState = MasterStateController.currentState.value
        scope.launch {
            try {
                status = "Midiendo LUFS de ${pair.a} (3 s)..."
                val lufsA = measureLufs(pair.a, 3.0, true)
                status = "Midiendo LUFS de ${pair.b} (3 s)..."
                val lufsB = measureLufs(pair.b, 3.0, true)
                if (lufsA.isNaN() || lufsB.isNaN()) {
                    status = "No se pudo medir LUFS (¿suena música?). Reproduce el pasaje y reintenta."
                    return@launch
                }
                // Target = estado A; trim de B para igualarlo.
                var trimB = LufsMatch.trimDb(lufsA, lufsB)
                runCatching { audioEngine.setMasterGain(trimB.toFloat()) }
                delay(500)
                val lufsB2 = measureLufs(pair.b, 3.0, false)
                var residual = LufsMatch.residualDb(lufsA, lufsB2, trimB)
                // Un paso de refinado si el residual supera la tolerancia.
                if (kotlin.math.abs(residual) > LufsMatch.TOLERANCE_LUFS) {
                    trimB += lufsA - lufsB2
                    runCatching { audioEngine.setMasterGain(trimB.toFloat()) }
                    delay(500)
                    val lufsB3 = measureLufs(pair.b, 3.0, false)
                    residual = LufsMatch.residualDb(lufsA, lufsB3, trimB)
                }
                trims = mapOf(pair.a to 0.0, pair.b to trimB)
                val matched = kotlin.math.abs(residual) <= LufsMatch.TOLERANCE_LUFS
                session.setLufsMatched(pair, matched)
                lufsText = "${pair.a}: ${"%.2f".format(lufsA)} LUFS · ${pair.b}: ${"%.2f".format(lufsB)} LUFS · trim ${pair.b}: ${"%.2f".format(trimB)} dB · residual ${"%.3f".format(residual)} LU"
                status = if (matched) "Igualado a ±0.05 LU. Puedes iniciar el ABX del par."
                    else "Residual ${"%.3f".format(residual)} LU > ±0.05. Reintenta con el mismo pasaje."
            } finally { busy = false }
        }
    }

    fun listen(sample: String) {
        val pair = currentPair ?: return
        if (busy) return
        busy = true
        readyToSubmit = false
        scope.launch {
            try {
                val state = session.stateFor(pair, sample)
                MasterStateController.submitState(state, "abx")
                val trim = trims[state] ?: 0.0
                runCatching { audioEngine.setMasterGain(trim.toFloat()) }
                delay(300)
                session.confirm(pair, sample, true)
                currentSample = sample
                readyToSubmit = session.canSubmit(pair, "A")
                status = "Muestra $sample preparada (${state}). Escucha antes de responder."
            } finally { busy = false }
        }
    }

    fun submitAnswer() {
        val pair = currentPair ?: return
        val sel = selectedMatch ?: return
        val correct = session.finish(pair, sel)
        AbxResultStore.record(context, session.trialJson(pair, Date().time))
        val next = session.currentPair
        currentPair = next
        selectedMatch = null
        readyToSubmit = false
        runCatching { audioEngine.setMasterGain(0.0f) }
        if (next == null) {
            phase = "done"
            status = "ABX completo: ${session.totalHits}/3 aciertos (p=${"%.4f".format(session.pValue())})."
        } else {
            phase = "match"
            status = "Par ${pair.name}: ${if (correct) "correcto" else "fallo"}. Iguala LUFS del siguiente par."
        }
    }

    Column(
        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text("ABX Estado maestro — BYPASS / LIMPIO / ACTIVO", style = MaterialTheme.typography.titleLarge)
        Text("Protocolo ABX_MasterState_Bypass_vs_Limpio_vs_Activo v1 · matched-LUFS ±0.05", style = MaterialTheme.typography.bodySmall)
        Text(status, style = MaterialTheme.typography.bodySmall)
        if (lufsText.isNotEmpty()) Text(lufsText, style = MaterialTheme.typography.bodySmall)
        Spacer(modifier = Modifier.height(24.dp))

        when (phase) {
            "match" -> {
                val pair = currentPair
                if (pair != null) {
                    Text("Par ${session.pairOrder.indexOf(pair) + 1}/3: ${pair.a} vs ${pair.b}", style = MaterialTheme.typography.titleMedium)
                    Spacer(modifier = Modifier.height(16.dp))
                    Button(enabled = !busy, onClick = { runLufsMatch(pair) }) {
                        Text("Igualar LUFS (±0.05)")
                    }
                    Spacer(modifier = Modifier.height(16.dp))
                    Button(
                        enabled = !busy && session.isLufsMatched(pair),
                        onClick = {
                            if (session.beginPair(pair)) {
                                phase = "abx"
                                selectedMatch = null
                                listen("A")
                            } else status = "El par no está igualado en LUFS. Pulsa «Igualar LUFS» primero."
                        }
                    ) { Text("Iniciar ABX del par") }
                }
            }
            "abx" -> {
                Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    Button(enabled = !busy, onClick = { listen("A") }, colors = ButtonDefaults.buttonColors(containerColor = if (currentSample == "A") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) { Text("Escuchar A") }
                    Button(enabled = !busy, onClick = { listen("B") }, colors = ButtonDefaults.buttonColors(containerColor = if (currentSample == "B") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) { Text("Escuchar B") }
                    Button(enabled = !busy, onClick = { listen("X") }, colors = ButtonDefaults.buttonColors(containerColor = if (currentSample == "X") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) { Text("Escuchar X") }
                }
                Spacer(modifier = Modifier.height(32.dp))
                Text("X es igual a:")
                Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    Button(onClick = { selectedMatch = "A" }, colors = ButtonDefaults.buttonColors(containerColor = if (selectedMatch == "A") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) { Text("Muestra A") }
                    Button(onClick = { selectedMatch = "B" }, colors = ButtonDefaults.buttonColors(containerColor = if (selectedMatch == "B") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) { Text("Muestra B") }
                }
                Spacer(modifier = Modifier.height(32.dp))
                Button(
                    onClick = { submitAnswer() },
                    enabled = !busy && readyToSubmit && currentPair?.let { session.canSubmit(it, selectedMatch) } == true
                ) { Text("Enviar y registrar") }
            }
            "done" -> {
                val p = session.pValue()
                val (ciLo, ciHi) = AbxStats.wilsonInterval95(session.totalHits, session.totalPairs)
                Card(modifier = Modifier.fillMaxWidth()) {
                    Column(modifier = Modifier.padding(16.dp)) {
                        Text("ABX estado maestro — resultado", style = MaterialTheme.typography.titleMedium)
                        Text("Aciertos: ${session.totalHits}/${session.totalPairs}")
                        Text("p-valor (binomial exacto, H0=0.5): ${"%.4f".format(p)}")
                        Text("IC 95%: [${"%.2f".format(ciLo)}, ${"%.2f".format(ciHi)}]")
                        Text(
                            if (session.totalPairs == 3 && p < 0.05) "Diferenciación reportada (p<0.05); no certifica calidad"
                            else "VEREDICTO: NO DEMOSTRADO",
                            color = if (session.totalPairs == 3 && p < 0.05)
                                MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.error
                        )
                        Spacer(modifier = Modifier.height(8.dp))
                        Button(onClick = {
                            lastExport = AbxResultStore.exportJson(context)?.absolutePath
                        }) { Text("Exportar JSON") }
                        lastExport?.let { Text("→ $it", style = MaterialTheme.typography.bodySmall) }
                    }
                }
            }
        }

        Spacer(modifier = Modifier.height(24.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            Button(onClick = { restore(); onBackToHrtf() }) { Text("Volver a ABX HRTF") }
            Button(onClick = { restore(); onBack() }) { Text("Volver") }
        }
    }
}

// ===========================================================================
// OrbeHolosAbxScreen — v2.5.471 (Repair 6 / F1): ABX ORBE·Ω+HOLOS·Ω ON vs OFF.
//
// CÓMO CORRERLO (el dueño corre 2 sesiones):
//   1. Pon música a sonar (el mismo pasaje de referencia en toda la
//      sesión; Tidal/Qobuz en la app nativa o el reproductor propio —
//      lo que suene por la cadena viva). El estado maestro debe estar
//      en ACTIVO y los controles FUSIÓN guardados deben tener
//      orbeAmount > 0 o holosAmount > 0 (si ambos están en 0, ON sería
//      idéntico a OFF y la pantalla no arranca: súbelos primero en
//      Operadores FUSIÓN).
//   2. Abre la pantalla: Hub → PRUEBA ABX → botón
//      «ABX ORBE·Ω+HOLOS·Ω — ON vs OFF, 16 ensayos (±0.2 LU)».
//   3. Pulsa «Igualar LUFS (±0.2 LU)»: mide el LUFS de OFF y de ON
//      (3 s por condición, mismo pasaje) y aplica el trim a ON con el
//      master gain. Si el residual queda > ±0.2 LU, reintenta con el
//      mismo pasaje. Sin igualado no se puede iniciar (fail-closed).
//   4. Pulsa «Iniciar ABX»: suena la muestra A del ensayo 1. Escucha
//      A, B y X (cada botón conmuta la condición en vivo; la pantalla
//      NUNCA muestra cuál es ON y cuál es OFF — ni en el texto de
//      estado). Responde «X es igual a A o a B» y pulsa «Enviar y
//      registrar». El ensayo queda guardado en AbxResultStore y avanza
//      al siguiente (16 en total).
//   5. Al terminar los 16 ensayos aparece el resumen: aciertos/16,
//      p-valor (binomial exacto, H0=0.5), IC 95% (Wilson) y el
//      veredicto («Diferenciación reportada» solo si n>=10 y p<0.05;
//      si no, «NO DEMOSTRADO»). «Exportar JSON» escribe el historial
//      completo (esta sesión incluida, en «history»).
//   6. Al salir se restauran tus controles FUSIÓN guardados y el master
//      gain vuelve a 0 dB. Los controles NO se sobrescriben durante el
//      ABX: el ON/OFF es transitorio y nunca se guarda en ParameterStore.
//
// CIEGO: la condición de cada muestra se sortea por ensayo
// (OrbeHolosAbxSession: un AbxTrialSession por ensayo). La pantalla no
// revela la condición ni el acierto/fallo por ensayo — solo el total al
// final — para no sesgar los ensayos restantes.
//
// AudioEngine SIN initialize(): libLoaded es companion (compartido con
// el motor vivo); getLufs() lee el medidor LUFS global vivo y
// setMasterGain() escribe el master gain global. NO llamar a
// initialize() aquí: re-correría nativeInit sobre el motor vivo.
//
// Validación en dispositivo pendiente: Kotlin no compila en host (sin
// SDK); verificar en el G85 que la pantalla abre, que el igualado LUFS
// llega a ±0.2 LU, que los 16 ensayos registran y que al salir los
// controles vuelven a los guardados.
// ===========================================================================
@Composable
fun OrbeHolosAbxScreen(onBack: () -> Unit, onBackToHrtf: () -> Unit) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val scope = rememberCoroutineScope()
    val session = remember { OrbeHolosAbxSession() }
    val audioEngine = remember { AudioEngine() }
    val fusedStore = remember { ParameterStore(context.applicationContext) }
    // ON = controles FUSIÓN guardados del usuario (tal cual). OFF = los
    // mismos controles con orbeAmount = 0 y holosAmount = 0; todo lo demás
    // idéntico, así que la única diferencia audible es ORBE·Ω+HOLOS·Ω.
    val baseControls = remember { fusedStore.loadFusedControls() }
    val offControls = remember(baseControls) {
        baseControls.copy(
            spatial = baseControls.spatial.copy(amount = 0.0f),
            field = baseControls.field.copy(amount = 0.0f),
        )
    }
    // Si el usuario ya tiene ORBE y HOLOS en 0, ON sería idéntico a OFF:
    // el estudio no mediría nada. Fail-closed en runLufsMatch().
    val onIsAudible = baseControls.spatial.amount > 0.0f ||
        baseControls.field.amount > 0.0f

    var status by remember { mutableStateOf("Reproduce el mismo pasaje de referencia y pulsa «Igualar LUFS».") }
    var busy by remember { mutableStateOf(false) }
    var phase by remember { mutableStateOf("match") } // match | abx | done
    var currentSample by remember { mutableStateOf("A") }
    var selectedMatch by remember { mutableStateOf<String?>(null) }
    var readyToSubmit by remember { mutableStateOf(false) }
    var lufsText by remember { mutableStateOf("") }
    var lastExport by remember { mutableStateOf<String?>(null) }
    var trims by remember { mutableStateOf<Map<OrbeHolosCondition, Double>>(emptyMap()) }
    // Historial guardado de ESTE protocolo (todas las sesiones del dueño).
    var historyTrials by remember { mutableStateOf(0) }
    var historyHits by remember { mutableStateOf(0) }

    fun refreshHistory() {
        val trials = AbxResultStore.loadAll(context).filter {
            it.optString("protocol") == OrbeHolosAbx.PROTOCOL &&
                it.optInt("protocolVersion") == OrbeHolosAbx.PROTOCOL_VERSION &&
                it.optBoolean("valid")
        }
        historyTrials = trials.size
        historyHits = trials.count { it.optBoolean("correct") }
    }
    LaunchedEffect(Unit) { refreshHistory() }

    // Conmuta la condición en vivo (Ruta A por JNI + Ruta B por
    // SET_PERCEPTUAL_STATE, dentro de safeSetFusedControls). NO guarda en
    // ParameterStore: el ON/OFF es transitorio; los controles del usuario
    // se restauran al salir con restore().
    fun applyCondition(condition: OrbeHolosCondition) {
        FusedControlsBridge.safeSetFusedControls(
            if (condition == OrbeHolosCondition.ON) baseControls else offControls
        )
    }

    fun restore() {
        FusedControlsBridge.safeSetFusedControls(baseControls)
        runCatching { audioEngine.setMasterGain(0.0f) }
    }
    DisposableEffect(Unit) { onDispose { restore(); session.cancelTrial() } }

    // Mide el LUFS (promedio en dominio lineal) de una condición durante
    // `seconds`. Con resetGain=true mide SIN trim (línea base); con
    // false mide CON el trim ya aplicado (verificación del residual).
    // El medidor lee ANTES del master gain (audio_orchestrator.cpp), así
    // que el residual efectivo es medido + trim (LufsMatch.residualDb).
    suspend fun measureLufs(condition: OrbeHolosCondition, seconds: Double, resetGain: Boolean): Double {
        applyCondition(condition)
        if (resetGain) runCatching { audioEngine.setMasterGain(0.0f) }
        delay(500)
        val samples = mutableListOf<Double>()
        repeat((seconds * 4).toInt()) {
            val v = audioEngine.getLufs().toDouble()
            if (v.isFinite() && v > -70.0) samples.add(v)
            delay(250)
        }
        if (samples.isEmpty()) return Double.NaN
        val lin = samples.map { Math.pow(10.0, it / 10.0) }.average()
        return 10.0 * Math.log10(lin)
    }

    fun runLufsMatch() {
        if (busy) return
        if (MasterStateController.currentState.value != MasterState.ACTIVO) {
            status = "El estado maestro no está en ACTIVO: pon la cadena en ACTIVO antes de igualar (en BYPASS o LIMPIO_OFF el ON/OFF no suena)."
            return
        }
        if (!onIsAudible) {
            status = "ORBE y HOLOS ya están en 0 en tus controles guardados: ON sería idéntico a OFF. Sube orbeAmount o holosAmount en Operadores FUSIÓN y vuelve."
            return
        }
        busy = true
        scope.launch {
            try {
                status = "Midiendo LUFS de OFF (3 s)..."
                val lufsOff = measureLufs(OrbeHolosCondition.OFF, 3.0, true)
                status = "Midiendo LUFS de ON (3 s)..."
                val lufsOn = measureLufs(OrbeHolosCondition.ON, 3.0, true)
                if (lufsOff.isNaN() || lufsOn.isNaN()) {
                    status = "No se pudo medir LUFS (¿suena música?). Reproduce el pasaje y reintenta."
                    return@launch
                }
                // Target = OFF; trim de ON para igualarlo.
                var trimOn = LufsMatch.trimDb(lufsOff, lufsOn)
                runCatching { audioEngine.setMasterGain(trimOn.toFloat()) }
                delay(500)
                val lufsOn2 = measureLufs(OrbeHolosCondition.ON, 3.0, false)
                var residual = LufsMatch.residualDb(lufsOff, lufsOn2, trimOn)
                // Un paso de refinado si el residual supera ±0.2 LU.
                if (kotlin.math.abs(residual) > OrbeHolosAbx.TOLERANCE_LU) {
                    trimOn += lufsOff - lufsOn2
                    runCatching { audioEngine.setMasterGain(trimOn.toFloat()) }
                    delay(500)
                    val lufsOn3 = measureLufs(OrbeHolosCondition.ON, 3.0, false)
                    residual = LufsMatch.residualDb(lufsOff, lufsOn3, trimOn)
                }
                trims = mapOf(OrbeHolosCondition.OFF to 0.0, OrbeHolosCondition.ON to trimOn)
                val matched = kotlin.math.abs(residual) <= OrbeHolosAbx.TOLERANCE_LU
                session.setLufsMatched(matched, trimOn, residual)
                lufsText = "OFF: ${"%.2f".format(lufsOff)} LUFS · ON: ${"%.2f".format(lufsOn)} LUFS · trim ON: ${"%.2f".format(trimOn)} dB · residual ${"%.3f".format(residual)} LU"
                status = if (matched) "Igualado a ±0.2 LU. Puedes iniciar el ABX (16 ensayos)."
                    else "Residual ${"%.3f".format(residual)} LU > ±0.2. Reintenta con el mismo pasaje."
            } finally { busy = false }
        }
    }

    fun listen(sample: String) {
        if (busy) return
        busy = true
        readyToSubmit = false
        scope.launch {
            try {
                // CIEGO: la condición se aplica pero NUNCA se muestra.
                val condition = session.conditionFor(sample)
                applyCondition(condition)
                val trim = trims[condition] ?: 0.0
                runCatching { audioEngine.setMasterGain(trim.toFloat()) }
                delay(300)
                session.confirm(sample, true)
                currentSample = sample
                readyToSubmit = session.canSubmit("A")
                status = "Muestra $sample preparada. Escucha antes de responder."
            } finally { busy = false }
        }
    }

    fun submitAnswer() {
        val sel = selectedMatch ?: return
        if (busy) return
        val trialNumber = session.currentTrialNumber
        session.finish(sel)
        AbxResultStore.record(context, session.trialJson(trialNumber, sel, Date().time))
        refreshHistory()
        selectedMatch = null
        readyToSubmit = false
        runCatching { audioEngine.setMasterGain(0.0f) }
        if (session.isDone) {
            phase = "done"
            status = "ABX completo: ${session.totalHits}/${OrbeHolosAbx.TRIAL_COUNT} aciertos (p=${"%.4f".format(session.pValue())})."
        } else {
            // Sin revelar acierto/fallo del ensayo: no sesgar los restantes.
            status = "Ensayo $trialNumber/${OrbeHolosAbx.TRIAL_COUNT} registrado."
            if (session.beginTrial()) listen("A")
        }
    }

    Column(
        modifier = Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center
    ) {
        Text("ABX ORBE·Ω+HOLOS·Ω — ON vs OFF", style = MaterialTheme.typography.titleLarge)
        Text("Protocolo ${OrbeHolosAbx.PROTOCOL} v${OrbeHolosAbx.PROTOCOL_VERSION} · ${OrbeHolosAbx.TRIAL_COUNT} ensayos · level-matched ±${OrbeHolosAbx.TOLERANCE_LU} LU", style = MaterialTheme.typography.bodySmall)
        Text(status, style = MaterialTheme.typography.bodySmall)
        if (lufsText.isNotEmpty()) Text(lufsText, style = MaterialTheme.typography.bodySmall)
        if (historyTrials > 0) Text("Historial guardado (todas las sesiones): $historyTrials ensayos · $historyHits aciertos", style = MaterialTheme.typography.bodySmall)
        Spacer(modifier = Modifier.height(24.dp))

        when (phase) {
            "match" -> {
                Text("Ensayos: ${session.totalTrials}/${OrbeHolosAbx.TRIAL_COUNT}", style = MaterialTheme.typography.titleMedium)
                Spacer(modifier = Modifier.height(16.dp))
                Button(enabled = !busy, onClick = { runLufsMatch() }) {
                    Text("Igualar LUFS (±0.2 LU)")
                }
                Spacer(modifier = Modifier.height(16.dp))
                Button(
                    enabled = !busy && session.isLufsMatched(),
                    onClick = {
                        if (session.beginTrial()) {
                            phase = "abx"
                            selectedMatch = null
                            listen("A")
                        } else status = "ON/OFF no están igualados en LUFS. Pulsa «Igualar LUFS» primero."
                    }
                ) { Text("Iniciar ABX (16 ensayos)") }
            }
            "abx" -> {
                Text("Ensayo ${session.currentTrialNumber}/${OrbeHolosAbx.TRIAL_COUNT}", style = MaterialTheme.typography.titleMedium)
                Spacer(modifier = Modifier.height(16.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    Button(enabled = !busy, onClick = { listen("A") }, colors = ButtonDefaults.buttonColors(containerColor = if (currentSample == "A") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) { Text("Escuchar A") }
                    Button(enabled = !busy, onClick = { listen("B") }, colors = ButtonDefaults.buttonColors(containerColor = if (currentSample == "B") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) { Text("Escuchar B") }
                    Button(enabled = !busy, onClick = { listen("X") }, colors = ButtonDefaults.buttonColors(containerColor = if (currentSample == "X") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) { Text("Escuchar X") }
                }
                Spacer(modifier = Modifier.height(32.dp))
                Text("X es igual a:")
                Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    Button(onClick = { selectedMatch = "A" }, colors = ButtonDefaults.buttonColors(containerColor = if (selectedMatch == "A") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) { Text("Muestra A") }
                    Button(onClick = { selectedMatch = "B" }, colors = ButtonDefaults.buttonColors(containerColor = if (selectedMatch == "B") MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.secondary)) { Text("Muestra B") }
                }
                Spacer(modifier = Modifier.height(32.dp))
                Button(
                    onClick = { submitAnswer() },
                    enabled = !busy && readyToSubmit && session.canSubmit(selectedMatch)
                ) { Text("Enviar y registrar") }
            }
            "done" -> {
                val p = session.pValue()
                val (ciLo, ciHi) = AbxStats.wilsonInterval95(session.totalHits, session.totalTrials)
                Card(modifier = Modifier.fillMaxWidth()) {
                    Column(modifier = Modifier.padding(16.dp)) {
                        Text("ABX ORBE·Ω+HOLOS·Ω — resultado", style = MaterialTheme.typography.titleMedium)
                        Text("Aciertos: ${session.totalHits}/${session.totalTrials}")
                        Text("p-valor (binomial exacto, H0=0.5): ${"%.4f".format(p)}")
                        Text("IC 95%: [${"%.2f".format(ciLo)}, ${"%.2f".format(ciHi)}]")
                        Text(
                            if (session.totalTrials >= 10 && p < 0.05) "Diferenciación reportada (p<0.05); no certifica calidad"
                            else "VEREDICTO: NO DEMOSTRADO",
                            color = if (session.totalTrials >= 10 && p < 0.05)
                                MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.error
                        )
                        if (historyTrials > session.totalTrials) {
                            Text("Historial guardado (todas las sesiones): $historyTrials ensayos · $historyHits aciertos", style = MaterialTheme.typography.bodySmall)
                        }
                        Spacer(modifier = Modifier.height(8.dp))
                        Button(onClick = {
                            lastExport = AbxResultStore.exportJson(context)?.absolutePath
                        }) { Text("Exportar JSON") }
                        lastExport?.let { Text("→ $it", style = MaterialTheme.typography.bodySmall) }
                        Text("El JSON exportado incluye el historial completo (esta sesión en «history»); el resumen de cabecera es del protocolo HRTF.", style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        }

        Spacer(modifier = Modifier.height(24.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            Button(onClick = { restore(); onBackToHrtf() }) { Text("Volver a ABX HRTF") }
            Button(onClick = { restore(); onBack() }) { Text("Volver") }
        }
    }
}
