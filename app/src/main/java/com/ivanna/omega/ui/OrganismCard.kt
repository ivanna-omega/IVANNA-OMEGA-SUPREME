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
    onSaveToken: (String) -> Unit,
    onClearQueue: () -> Unit = {},
    modifier: Modifier = Modifier
) {
    var tokenDraft by remember { mutableStateOf("") }
    var tokenSaved by remember { mutableStateOf(false) }
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
            if (state.micReanchors > 0) {
                Text(
                    "Mic re-anclado al interno ${state.micReanchors} vez/veces (DAC conectado).",
                    style = MaterialTheme.typography.bodySmall
                )
            }
            if (state.status == OrganismController.Status.NO_TOKEN ||
                state.status == OrganismController.Status.TOKEN_INVALID
            ) {
                Spacer(modifier = Modifier.height(8.dp))
                OutlinedTextField(
                    value = tokenDraft,
                    onValueChange = { tokenDraft = it; tokenSaved = false },
                    label = { Text("Token de Muselink (una vez)") },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    // Defecto #8: el token no se muestra en claro.
                    visualTransformation = PasswordVisualTransformation()
                )
                Spacer(modifier = Modifier.height(8.dp))
                Button(onClick = {
                    onSaveToken(tokenDraft)
                    tokenDraft = ""
                    tokenSaved = true
                }) { Text("Guardar token cifrado") }
                if (tokenSaved) {
                    Text(
                        "Token guardado. El envío se reintenta solo.",
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
