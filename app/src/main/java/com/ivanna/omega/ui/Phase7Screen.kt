package com.ivanna.omega.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ivanna.omega.spatial.AutoEqManager
import com.ivanna.omega.spatial.IvannaSpatialManager
import com.ivanna.omega.ui.theme.AuroraCyan
import com.ivanna.omega.ui.theme.GlassOutline
import com.ivanna.omega.ui.theme.ObsidianSoft
import com.ivanna.omega.ui.theme.ObsidianVoid
import com.ivanna.omega.ui.theme.TextMuted

/**
 * Fase 7: Hegemonía Total — v2.5.448 (refinamiento magistral + sin root).
 *
 * Directiva de Esse (2026-11-15): este sub-entorno no estaba refinado y debe
 * funcionar SIN root. Antes usaba el tema Material por defecto (morado) y no
 * dejaba claro que el AutoEQ funciona sin root.
 *
 * Ahora:
 * - Tema IVANNA magistral (ObsidianVoid, AuroraCyan, tarjetas ObsidianSoft).
 * - AutoEQ explícito SIN ROOT: se aplica al renderer nativo EN PROCESO
 *   (IvannaSpatialManager.setAutoEq → motor de la app, Ruta A). No necesita
 *   daemon ni Magisk. El estado lo dice claro.
 * - ARCore sigue honesto ("No disponible"): el escaneo antropométrico real
 *   (MediaPipe/ARCore) no está implementado; la calibración real hoy es la
 *   manual (medidas de pinna → sujeto HRTF en la pantalla de calibración SAF).
 */
@Composable
fun Phase7Screen(onBack: () -> Unit) {
    val ctx = LocalContext.current
    // FIX (control muerto + sin persistencia): eqEnabled era un
    // mutableStateOf suelto — no llamaba a AutoEqManager (importado y
    // nunca usado) y se reseteaba a false al recomponer/reabrir.
    val prefs = remember {
        ctx.getSharedPreferences("ivanna_phase7_prefs", android.content.Context.MODE_PRIVATE)
    }
    val profile = remember { AutoEqManager.availableProfiles.first() }

    var eqEnabled by remember { mutableStateOf(prefs.getBoolean("autoEqEnabled", true)) }
    var eqApplied by remember { mutableStateOf(false) }

    // Reaplicar el estado guardado al motor nativo cuando el renderer
    // esté listo (init del SpatialManager es asíncrono).
    LaunchedEffect(eqEnabled) {
        // IvannaSpatialManager.ready no es un State observable, así que se
        // reintenta unos segundos en vez de fallar en silencio si el
        // renderer todavía se está creando en su hilo de init.
        var attempts = 0
        while (attempts < 20) {
            eqApplied = IvannaSpatialManager.setAutoEq(eqEnabled, profile)
            if (eqApplied) break
            attempts++
            kotlinx.coroutines.delay(500)
        }
    }

    Column(
        modifier = Modifier.fillMaxSize().background(ObsidianVoid).padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        Text(
            "Fase 7: Hegemonía Total",
            color = AuroraCyan, fontWeight = FontWeight.ExtraBold, fontSize = 18.sp
        )
        Text(
            "Calibración del audífono y visión computacional. El AutoEQ funciona sin root: se aplica al renderer nativo de la app.",
            color = TextMuted, fontSize = 13.sp
        )

        Column(
            Modifier.fillMaxWidth()
                .background(ObsidianSoft, RoundedCornerShape(14.dp))
                .border(1.dp, GlassOutline.copy(alpha = 0.5f), RoundedCornerShape(14.dp))
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text("Visión Computacional (ARCore)", color = AuroraCyan, fontWeight = FontWeight.Bold, fontSize = 15.sp)
            Text(
                "No disponible. La calibración antropométrica activa es la manual (medidas de pinna) en la pantalla de calibración SAF.",
                color = TextMuted, fontSize = 13.sp
            )
            Text(
                "Qué es: escaneo de la oreja por cámara para elegir el HRTF más cercano. Para qué sirve: personalizar el espacial a tu anatomía. Cuándo sí: cuando se implemente el escaneo real. Cuándo no: hoy usa la calibración manual de pinna en SAF.",
                color = TextMuted, fontSize = 12.sp
            )
        }

        Column(
            Modifier.fillMaxWidth()
                .background(ObsidianSoft, RoundedCornerShape(14.dp))
                .border(1.dp, GlassOutline.copy(alpha = 0.5f), RoundedCornerShape(14.dp))
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text("AutoEQ (Neutralización OEM)", color = AuroraCyan, fontWeight = FontWeight.Bold, fontSize = 15.sp)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Checkbox(
                    checked = eqEnabled,
                    onCheckedChange = { on ->
                        eqEnabled = on
                        prefs.edit().putBoolean("autoEqEnabled", on).apply()
                        eqApplied = IvannaSpatialManager.setAutoEq(on, profile)
                    },
                    colors = CheckboxDefaults.colors(
                        checkedColor = AuroraCyan,
                        uncheckedColor = TextMuted,
                        checkmarkColor = Color.Black
                    )
                )
                Text("Activar Calibración ($profile)", color = Color.White, fontWeight = FontWeight.Bold)
            }
            Text(
                when {
                    !eqEnabled -> "AutoEQ desactivado"
                    eqApplied  -> "Aplicado al renderer nativo de la app (bandas AutoEQ activas) — sin root"
                    else       -> "Guardado — pendiente: el renderer espacial aún no está listo"
                },
                color = TextMuted, fontSize = 13.sp
            )
            Text(
                "Qué es: neutraliza la respuesta propia del audífono (Sennheiser HD650) para que el espacial no herede su color. Para qué sirve: que el HRTF y la sala suenen como se diseñaron, no teñidos por el audífono. Cuándo sí: con el HD650 (u otro perfil, si lo cambias). Cuándo no: si usas otro audífono sin su perfil, apágalo.",
                color = TextMuted, fontSize = 12.sp
            )
        }

        Spacer(modifier = Modifier.weight(1f))
        Button(
            onClick = onBack,
            colors = ButtonDefaults.buttonColors(containerColor = AuroraCyan, contentColor = Color.Black)
        ) {
            Text("Volver", fontWeight = FontWeight.Bold)
        }
    }
}
