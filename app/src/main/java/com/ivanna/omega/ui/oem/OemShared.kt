package com.ivanna.omega.ui.oem

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ivanna.omega.ui.theme.*

@Composable
fun TelRow(label: String, value: String, color: Color) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(label, color = TextMuted, fontSize = 9.sp, modifier = Modifier.weight(1f))
        Text(value, color = color, fontSize = 9.sp,
            fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Medium)
    }
}

/**
 * Glosario de siglas en palabras simples (inventario Apéndice B): se muestra
 * JUNTO a los medidores/chips que usan las siglas, no en otra pantalla.
 * Nada que memorizar: cada sigla dice qué significa en una línea.
 */
@Composable
fun OemGlossaryNote(text: String) {
    Text(
        text, color = TextMuted, fontSize = 8.sp, lineHeight = 12.sp,
        modifier = Modifier.fillMaxWidth().padding(top = 6.dp)
    )
}

@Composable
fun OemSliderRow(
    label: String,
    value: Float,
    min: Float,
    max: Float,
    display: String,
    onValueChange: (Float) -> Unit
) {
    val safeValue = com.ivanna.omega.safmus.SafmusUniversalKernel
        .sanitizeControlValue(value, min, max) ?: min
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically) {
            Text(label, color = TextSecondary, fontSize = 9.sp)
            Text(display, color = AuroraCyan, fontSize = 9.sp,
                fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold)
        }
        Slider(
            value = safeValue,
            onValueChange = { proposed ->
                com.ivanna.omega.safmus.SafmusUniversalKernel
                    .sanitizeControlValue(proposed, min, max)?.let(onValueChange)
            },
            valueRange = min..max,
            modifier = Modifier.fillMaxWidth().height(28.dp),
            colors = SliderDefaults.colors(
                thumbColor = AuroraCyan,
                activeTrackColor = AuroraCyan,
                inactiveTrackColor = ObsidianEdge
            )
        )
    }
}
