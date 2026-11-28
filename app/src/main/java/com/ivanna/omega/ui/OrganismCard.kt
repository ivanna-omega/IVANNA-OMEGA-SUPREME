package com.ivanna.omega.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.ivanna.omega.organism.OrganismController
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

/**
 * OrganismCard — la UI MÍNIMA indispensable del organismo (v2.5.506).
 *
 * UN interruptor maestro ("Organismo automático: ON/OFF") + UN indicador
 * de estado (Capturando / En cola / Sin conexión / Error recuperable /
 * Apagado / Sin token / Token inválido). Nada más.
 *
 * El campo del token SOLO aparece cuando el estado es Sin token o Token
 * inválido (provisionamiento de una vez, no es UI por sesión): ahí Esse
 * pega el MUSE_CONNECTOR_TOKEN de Muselink y se guarda cifrado
 * (EncryptedSharedPreferences, nunca en logs). Sin token, la cola se
 * conserva local y no se intenta la red.
 *
 * El texto deja claros los límites: RECORD_AUDIO concedido una vez
 * (revocable), y si el token expira hay que reautorizar una vez.
 */
@Composable
fun OrganismCard(
    state: OrganismController.State,
    onToggle: (Boolean) -> Unit,
    onSaveToken: (String) -> Boolean,
    onClearQueue: () -> Unit = {},
    onDiagnoseToken: () -> Unit = {},
    // C22/510: borrado explícito del perfil estable. Solo se muestra con
    // el interruptor maestro en OFF; el controlador lo rechaza si está ON.
    onDeleteStableProfile: () -> Unit = {},
    modifier: Modifier = Modifier
) {
    var tokenDraft by remember { mutableStateOf("") }
    var tokenSaved by remember { mutableStateOf(false) }
    var tokenSaveFailed by remember { mutableStateOf(false) }
    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant
        )
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        "Organismo automático",
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold
                    )
                    Text(
                        "El mic escucha la sala y la sesión viaja sola al organismo. Apagarlo corta todo envío.",
                        style = MaterialTheme.typography.bodySmall
                    )
                }
                Spacer(modifier = Modifier.width(12.dp))
                Switch(checked = state.enabled, onCheckedChange = { onToggle(it) })
            }
            Spacer(modifier = Modifier.height(8.dp))
            Text(
                "Estado: ${statusText(state.status)}",
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.SemiBold
            )
            Text(state.detail, style = MaterialTheme.typography.bodySmall)
            // Defecto #11: lectura continua del dispositivo enrutado
            // (no solo el contador cuando hay re-anclas).
            Text(
                "Mic: ${state.micRoutedDevice}",
                style = MaterialTheme.typography.bodySmall,
                fontWeight = FontWeight.SemiBold
            )
            if (state.queued > 0) {
                Text(
                    "En cola: ${state.queued} chunk(s) · Enviados: ${state.sentTotal}",
                    style = MaterialTheme.typography.bodySmall
                )
                Spacer(modifier = Modifier.height(4.dp))
                Button(onClick = onClearQueue) { Text("Borrar cola local") }
            }
            // C22/510: el estable (último perfil bueno) solo se borra con
            // borrado explícito y el organismo apagado; borrar la cola no
            // lo toca. Sin este botón no habría forma de borrarlo.
            if (!state.enabled) {
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    "Borrar la cola conserva el último perfil bueno. Para borrarlo también:",
                    style = MaterialTheme.typography.bodySmall
                )
                Spacer(modifier = Modifier.height(4.dp))
                Button(onClick = onDeleteStableProfile) { Text("Borrar perfil estable") }
            }
            if (state.micReanchors > 0) {
                Text(
                    "Mic re-anclado al interno ${state.micReanchors} vez/veces (DAC conectado).",
                    style = MaterialTheme.typography.bodySmall
                )
            }
            // C23/510 — salud del consumidor: solo se muestra cuando hay
            // algo que ver (rechazos o stalls); con todo a cero no añade
            // ruido. Un stall con música sonando es «el sensor se paró y
            // nadie lo notaba»: el estado diría «Capturando» o «En
            // espera» mientras la sesión no se genera.
            if (state.snapshotsRejected > 0 || state.sensorStalls > 0) {
                Text(
                    "Sensor: ${state.snapshotsAccepted} aceptados · ${state.snapshotsRejected} rechazados · ${state.sensorStalls} stall(s).",
                    style = MaterialTheme.typography.bodySmall
                )
                if (state.lastSensorStallAtMs > 0L) {
                    Text(
                        "Último stall: ${format401Date(state.lastSensorStallAtMs)} (frameIndex parado con música; mic re-anclado).",
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            }
            // C21/510 — diagnóstico de un toque del token: cuando el
            // estado huele a token o a red (Sin token / Token inválido /
            // Sin conexión / Error), un toque dice si los hosts
            // responden y si el DNS directo resuelve, para que el
            // re-pegado sea de un minuto y no una investigación. No
            // envía el token (el GET va sin Authorization).
            if (state.status == OrganismController.Status.NO_TOKEN ||
                state.status == OrganismController.Status.TOKEN_INVALID ||
                state.status == OrganismController.Status.OFFLINE ||
                state.status == OrganismController.Status.ERROR
            ) {
                Spacer(modifier = Modifier.height(8.dp))
                Button(
                    onClick = onDiagnoseToken,
                    enabled = !state.tokenDiagnosing
                ) {
                    Text(if (state.tokenDiagnosing) "Diagnosticando…" else "Diagnosticar conexión")
                }
            }
            if (state.last401AtMs > 0L) {
                Text(
                    "Último 401: ${format401Date(state.last401AtMs)} (el token de entonces murió; si la red vive, pega uno nuevo).",
                    style = MaterialTheme.typography.bodySmall
                )
            }
            state.lastTokenDiagnosis?.let { diagnosis ->
                Text(
                    diagnosis.summary(),
                    style = MaterialTheme.typography.bodySmall
                )
                if (!diagnosis.anyReachable) {
                    Text(
                        "Ningún host responde: es la red, no el token. Revisa la conexión antes de re-pegar.",
                        style = MaterialTheme.typography.bodySmall,
                        fontWeight = FontWeight.SemiBold
                    )
                }
            }
            if (state.status == OrganismController.Status.NO_TOKEN ||
                state.status == OrganismController.Status.TOKEN_INVALID
            ) {
                Spacer(modifier = Modifier.height(8.dp))
                OutlinedTextField(
                    value = tokenDraft,
                    onValueChange = { tokenDraft = it; tokenSaved = false; tokenSaveFailed = false },
                    label = { Text("Token de Muselink (una vez)") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    // Defecto #8: el token no se muestra en claro.
                    visualTransformation = PasswordVisualTransformation()
                )
                Spacer(modifier = Modifier.height(8.dp))
                Button(onClick = {
                    // El resultado real del guardado: antes se mostraba
                    // "Token guardado" aunque el guardado fallara (el
                    // Boolean se descartaba). Solo se limpia el campo si
                    // se guardó de verdad; si falló, el texto queda para
                    // que Esse lo revise.
                    val ok = onSaveToken(tokenDraft)
                    if (ok) {
                        tokenDraft = ""
                        tokenSaved = true
                        tokenSaveFailed = false
                    } else {
                        tokenSaved = false
                        tokenSaveFailed = true
                    }
                }) { Text("Guardar token cifrado") }
                if (tokenSaved) {
                    Text(
                        "Token guardado. El envío se reintenta solo.",
                        style = MaterialTheme.typography.bodySmall
                    )
                }
                if (tokenSaveFailed) {
                    Text(
                        "No se pudo guardar el token. Revisa el valor.",
                        style = MaterialTheme.typography.bodySmall
                    )
                }
            }
            Text(
                "Límites: requiere permiso de micrófono (revocable); si el token expira, se reautoriza una vez. El audio nunca depende del envío.",
                style = MaterialTheme.typography.bodySmall
            )
        }
    }
}

fun statusText(status: OrganismController.Status): String = when (status) {
    OrganismController.Status.OFF -> "Apagado"
    OrganismController.Status.CAPTURING -> "Capturando"
    OrganismController.Status.QUEUED -> "En cola"
    OrganismController.Status.OFFLINE -> "Sin conexión"
    OrganismController.Status.ERROR -> "Error recuperable"
    OrganismController.Status.NO_TOKEN -> "Sin token"
    OrganismController.Status.TOKEN_INVALID -> "Token inválido"
}

/** C21: fecha del último 401 en hora local (día + hora, sin segundos). */
private val formatter401: DateTimeFormatter =
    DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm").withZone(ZoneId.systemDefault())

fun format401Date(epochMs: Long): String =
    Instant.ofEpochMilli(epochMs).atZone(ZoneId.systemDefault()).format(formatter401)
