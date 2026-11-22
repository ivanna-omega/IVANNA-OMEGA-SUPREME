package com.ivanna.omega.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

private data class ControlHelpEntry(val name: String, val purpose: String, val use: String)
private data class ControlHelpGroup(val title: String, val entries: List<ControlHelpEntry>)

private val controlHelpGroups = listOf(
    ControlHelpGroup("CONTROL · SONIDO PRINCIPAL", listOf(
        ControlHelpEntry("Modo DSP / +NHO / +Spatial / +HRTF", "Selecciona cuánto procesamiento se incorpora a la ruta de IVANNA.", "Empieza en DSP y añade espacialidad o HRTF solo si la ruta activa y el dispositivo lo admiten."),
        ControlHelpEntry("Modo automático", "Permite que los módulos adaptativos ajusten parámetros compatibles según el estado disponible.", "Úsalo para escuchar sin mover controles continuamente; el modo manual es mejor para comparar cambios concretos."),
        ControlHelpEntry("Perfil o preset", "Carga un conjunto de ajustes pensado para una intención de escucha.", "Compara un preset cada vez y vuelve al perfil neutro si notas distorsión o fatiga."),
        ControlHelpEntry("Exciter / armónicos", "Añade armónicos para modificar la presencia y el brillo percibidos.", "Aumenta poco a poco; demasiado puede volver ásperas las voces, los platillos y las sibilantes."),
        ControlHelpEntry("Ecualización (EQ)", "Atenúa o realza zonas de frecuencia.", "Corrige primero problemas evidentes de los audífonos o altavoces; evita grandes realces que consuman margen."),
        ControlHelpEntry("Compresor: umbral y relación", "El umbral determina cuándo actúa; la relación determina cuánto reduce los picos que lo superan.", "Para música dinámica, usa compresión suave. Si todo suena aplastado, reduce la compresión."),
        ControlHelpEntry("AntiDolby", "Activa la etapa de compatibilidad/procesamiento indicada por el motor IVANNA.", "Compara activado y desactivado con el mismo volumen; no presupongas que mejora todos los contenidos."),
        ControlHelpEntry("Anchura estéreo", "Modifica la sensación de amplitud entre los canales.", "En audífonos, evita exagerarla si las voces pierden el centro; en estéreo, comprueba que el mono siga siendo claro."),
        ControlHelpEntry("Hi-Res", "Abre los ajustes relacionados con reproducción de alta resolución disponibles en la app.", "La etiqueta no garantiza que el archivo, Android, la ruta y el DAC estén funcionando a una frecuencia concreta; verifica la ruta real."),
        ControlHelpEntry("EQ / COMP", "Abre los controles detallados de ecualización y compresión.", "Ajusta una banda o parámetro por vez y compara con bypass al mismo nivel percibido."),
        ControlHelpEntry("Telemetría", "Muestra las mediciones que el motor realmente puede proporcionar.", "Un guion o «no disponible» significa que no hay dato válido; no equivale a cero."),
        ControlHelpEntry("Perfiles adaptativos", "Abre los perfiles que modifican el comportamiento según el contexto.", "Si prefieres un sonido constante, desactiva la adaptación y guarda una referencia manual.")
    )),
    ControlHelpGroup("FÍSICA NUEVA · Ω·Σ·LGN·GENEZI", listOf(
        ControlHelpEntry("Interruptor maestro Ω", "Enciende o apaga toda la física nueva. En 0 el motor hace bypass bit-exacto (la señal pasa sin tocarse).", "Apágalo para comparar el sonido con y sin física nueva al mismo volumen; déjalo encendido para el sonido magistral."),
        ControlHelpEntry("Mezcla Maestra", "Cuánto del motor nuevo se mezcla con la señal (0–1.5; 1.0 = 100%, hasta 1.5 = overdrive que extrapola la diferencia wet−dry).", "Déjalo en 1.0. El overdrive es solo para pruebas: pasado 1.0 el motor exagera, no mejora."),
        ControlHelpEntry("LGN 3D Pinna", "Mezcla del modelo de pabellón auricular (LGN) que coloca el sonido en 3D, fuera de la cabeza (0–1, default 1.0).", "En 1.0 el efecto es pleno; bájalo si la imagen se siente exagerada o hueca."),
        ControlHelpEntry("Amortiguación Bright-Spot", "Amortigua el punto brillante donde la HRTF concentra energía y suena áspero (0–1, default 0.65).", "Súbelo si notas aspereza en agudos; bájalo si el sonido pierde aire."),
        ControlHelpEntry("Eco de Torso", "Retardo del torso: la reflexión del torso llega un poco después y da cuerpo a la localización (0–0.5, default 0.18).", "Déjalo bajo; subirlo demasiado ensucia la imagen."),
        ControlHelpEntry("Azimut Ω", "Coloca la fuente virtual a izquierda o derecha (−180°..180°, default 0°).", "0° = centro. Mueve de a poco para posicionar una fuente."),
        ControlHelpEntry("Elevación Ω", "Sitúa la fuente virtual por encima o por debajo del plano de los oídos (−40°..70°, default 0°).", "0° = plano de los oídos; sube para fuentes altas."),
        ControlHelpEntry("Tope de Refuerzo Ω", "Límite de cuánto puede reforzar el motor (0–9 dB, default 3.0 dB).", "3 dB es el tope seguro; subirlo da más presencia pero arriesga picos."),
        ControlHelpEntry("Compensación de Aire", "Compensa la absorción de agudos del aire a distancia (0–1, default 0.0).", "En sala pequeña déjalo en 0; súbelo solo a distancias grandes."),
        ControlHelpEntry("Radio Cabeza Personal", "Radio de cabeza que usa el modelo para el tiempo entre oídos ITD (6.5–11.5 cm, default 8.75 cm).", "El default es el promedio; ajústalo solo si mediste tu cabeza."),
        ControlHelpEntry("Ω Firma Élite Harman2018 − H_b", "Mezcla la curva de referencia élite Harman 2018 (0–1, default 0.0 = neutro).", "Súbelo si quieres el tinte de la firma élite; en 0 el motor no la aplica."),
        ControlHelpEntry("Pre-Compensación Diafonía Ec.7", "Cancela la diafonía antes de que ocurra: el sonido de la bocina izquierda que llega al oído derecho (0–1.5, default 1.0).", "1.0 en bocinas (Genezi); con audífonos no hay diafonía que cancelar."),
        ControlHelpEntry("Σ Distancia / Escala", "Distancia bocinas→cabeza que usa el modelo Σ (0.15–5.0 m, default 3.5 m, la distancia real al Genezi).", "Pon la distancia real. Un valor absurdo (15.79 m) causaba el piiiii; en este teléfono Esse lo tiene en 0.35 m por su orden."),
        ControlHelpEntry("Σ Nivel SPL Estimado", "Nivel de escucha que el motor asume para la no-linealidad Klippel (65–115 dB, default 85 dB).", "Pon tu nivel real de escucha; 85 dB es el de referencia."),
        ControlHelpEntry("Genezi Volterra Laguerre H2", "Pre-distorsión que cancela la distorsión H2 del Genezi con kernel Laguerre (polo 0.9983; H2 0.50% aproximado, 0–1, default 1.0).", "1.0 con el Genezi; 0 si no hay kernel cargado."),
        ControlHelpEntry("Lazo Variacional F · MOC + Onsets", "Lazo de energía libre: el motor ajusta MOC y onsets según la señal (0–1, default 0.0 = apagado).", "Experimental; déjalo en 0 salvo pruebas controladas."),
        ControlHelpEntry("Equipo Ω", "Clase de equipo que el motor asume: audífonos, bocina G85, BT, Genezi PZ1D, auto, PA/club, estadio (E5 no existe).", "Elige el equipo real que suena; cada uno cambia la física. Genezi = E4."),
        ControlHelpEntry("ABX", "Prueba ciega: A = bypass, B = física nueva (Off → A → B → Off).", "Úsalo para comparar sin sesgo al mismo volumen.")
    )),
    ControlHelpGroup("FÍSICA NUEVA v3 · ÓRGANOS DESPIERTOS", listOf(
        ControlHelpEntry("Sub-graves modal (cut-only)", "5 etapas que solo cortan (nunca refuerzan) los picos de graves: polo Genezi 68.5 Hz + 4 modos axiales de la sala (37.3, 49.0, 59.1, 74.6 Hz). Valores aproximados, no medidos.", "Déjalas activas con sus defaults; apaga una etapa solo si sabes que ese modo no existe en tu sala."),
        ControlHelpEntry("Frecuencia de etapa", "Frecuencia del modo que corta cada etapa (15–120 Hz).", "No la muevas salvo que hayas medido tu sala."),
        ControlHelpEntry("Q de etapa", "Qué tan estrecho es el corte (0.3–18; default 3.0–4.0).", "Q alto corta solo el pico; Q bajo corta más ancho."),
        ControlHelpEntry("Ganancia de etapa", "Cuánto corta (negativo) cada etapa (−12..+6 dB; default −3/−4 dB). La Fase 4 es cut-only: nunca refuerza.", "Déjala en negativo; nunca la subas a positivo sin medir."),
        ControlHelpEntry("Mezcla de cola", "Cuánta cola de sala real (RIR medida) se mezcla con el directo (0–1, default 0.5).", "0.5 = cola audible sin ahogar el directo; bájala si la grabación ya tiene reverberación."),
        ControlHelpEntry("Selector RIR", "Elige la sala real del dataset de 200 habitaciones; rir_0101 es la más parecida a la sala de Esse (3.5×2.9×4.6 m, RT60 0.34 s).", "Cambia de sala para probar otra acústica; el botón restablece rir_0101."),
        ControlHelpEntry("Mezcla H3", "Corrección del armónico 3 del Genezi (Klippel, H3 0.20% aproximado; 0–1, default 1.0).", "1.0 con el Genezi; 0 = sin corrección H3."),
        ControlHelpEntry("Temperatura de bobina", "Modelo térmico Klippel de la bobina (protección sobre 115 °C, límite 135 °C). Solo con equipo de bocinas; solo lectura.", "Si se acerca al límite, baja el volumen; no fuerces los límites para ganar volumen."),
        ControlHelpEntry("Ruta A / Ruta B (v3)", "Los sliders v3 mueven solo Ruta A en vivo; Ruta B suena con estos mismos defaults compilados hasta la extensión ABI del snapshot.", "Si un ajuste v3 no se oye en Tidal/Qobuz (Ruta B), es por esto, no porque el control esté roto."),
        ControlHelpEntry("Telemetría v3", "Cola activa/inactiva, sub-graves n/5, K2 MEDIDO/APROXIMADO/sin kernel, µs por hop. Solo lectura.", "K2 APROXIMADO significa kernel aproximado (H2 0.50%/H3 0.20%), no medido.")
    )),
    ControlHelpGroup("CONTROL · SALA, AUX Y CAMPO ESPACIAL", listOf(
        ControlHelpEntry("Medir sala", "Solicita una medición acústica con el micrófono para estimar correcciones compatibles.", "Hazla en silencio, con el teléfono en el punto de escucha y concede el permiso solo cuando se solicite."),
        ControlHelpEntry("Corrección de sala", "Aplica la corrección calculada cuando existe una medición válida.", "Úsala principalmente con altavoces en una sala; en audífonos, la medición de habitación no sustituye una calibración de audífonos."),
        ControlHelpEntry("Borrar medición", "Elimina los datos de medición de sala guardados por este flujo.", "Vuelve a medir si cambiaste de habitación, posición o sistema."),
        ControlHelpEntry("Campo AUX / crossover", "Activa el procesamiento de campo indicado para una salida por cable o amplificador.", "Empieza desactivado; actívalo solo si el sistema y el contenido se benefician. No reemplaza un crossover físico correctamente configurado."),
        ControlHelpEntry("Upmix HOA / inmersividad", "Intenta distribuir contenido estéreo en un campo espacial de orden superior.", "Úsalo para experimentar con espacialidad; para una mezcla estéreo fiel, compara también con el upmix apagado."),
        ControlHelpEntry("Dinámica espacial temporal", "Modula la amplitud espacial a lo largo de los transitorios y sus colas.", "Mantén una intensidad moderada para evitar que la escena parezca moverse de forma artificial."),
        ControlHelpEntry("Háptica de graves", "Coordina vibración del dispositivo compatible con la energía de graves detectada por el motor.", "Es opcional y depende del hardware; no aumenta los graves acústicos que llegan al oído."),
        ControlHelpEntry("WFS", "Activa el renderizador de síntesis de frente de onda para un arreglo virtual de altavoces.", "Es experimental fuera de un arreglo y ruta compatibles; con audífonos no crea por sí solo un frente de onda físico en la habitación."),
        ControlHelpEntry("Preset 7ch", "Envía la distribución de referencia de siete canales al motor WFS.", "Úsalo como punto de partida, no como calibración automática de una sala real."),
        ControlHelpEntry("Oyente X / Y / Z", "Define la posición de escucha a lo ancho, altura y profundidad de la sala.", "Mide aproximadamente la ubicación de tus oídos respecto a las paredes y el suelo."),
        ControlHelpEntry("Giro horizontal", "Rota el marco de escucha y recalcula las coordenadas relativas del arreglo virtual.", "Déjalo en 0° si miras hacia el frente de referencia; cambia el ángulo si estás orientado hacia otro lado.")
    )),
    ControlHelpGroup("SONIDO · PERCEPCIÓN Y EFECTOS", listOf(
        ControlHelpEntry("Tipo de contenido", "Selecciona una categoría para que el motor use el perfil correspondiente.", "Elige la categoría más cercana al contenido que escuchas; no es un control de volumen."),
        ControlHelpEntry("Loudness trim", "Compensa diferencias de sonoridad según la implementación disponible.", "Útil al escuchar a volumen bajo; desactívalo si altera demasiado el balance tonal."),
        ControlHelpEntry("HRTF", "Usa funciones relacionadas con la transferencia acústica de cabeza y oído para colocar sonidos en 3D.", "Es especialmente pertinente para audífonos. La precisión depende del perfil y de la ruta efectiva."),
        ControlHelpEntry("Adaptación binaural", "Permite adaptar parámetros del procesamiento binaural cuando hay datos válidos.", "Apágala para comparar una escena fija y repetible."),
        ControlHelpEntry("Órbita lenta 3D", "Introduce movimiento lento de la escena espacial.", "Es un efecto creativo; desactívalo para música que deba mantener una imagen estable."),
        ControlHelpEntry("Convolución híbrida + sala activa", "Combina convolución y componentes de sala habilitados por el motor.", "Puede añadir sensación de espacio y cola; reduce o apaga si la mezcla ya tiene reverberación abundante."),
        ControlHelpEntry("Anchura / ángulo binaural", "Ajusta la extensión y orientación de la imagen binaural.", "Usa cambios pequeños y conserva el centro vocal estable."),
        ControlHelpEntry("Inverse cochlear / cóclea", "Controla el procesamiento auditivo inverso que ofrece el módulo correspondiente.", "Es experimental: compara a nivel igualado y desactívalo ante fatiga, aspereza o pérdida de claridad.")
    )),
    ControlHelpGroup("BRAIN · ADAPTACIÓN Y LABORATORIO", listOf(
        ControlHelpEntry("Modo manual", "Evita que los controles compatibles cambien automáticamente.", "Actívalo para realizar pruebas A/B repetibles y desactívalo si quieres adaptación automática."),
        ControlHelpEntry("Población evolutiva", "Define cuántas soluciones candidatas usa el proceso evolutivo.", "Valores mayores pueden exigir más CPU y tiempo; no significan automáticamente mejor sonido."),
        ControlHelpEntry("Generaciones", "Define cuántas rondas de evolución se ejecutan.", "Aumenta solo si estás haciendo una experimentación controlada y puedes esperar el cálculo."),
        ControlHelpEntry("Iniciar / detener", "Arranca o detiene el proceso de laboratorio cuando el motor nativo está disponible.", "No lo confundas con iniciar la reproducción de música."),
        ControlHelpEntry("Reset", "Restablece el estado del experimento del laboratorio.", "Guarda primero cualquier resultado que quieras conservar."),
        ControlHelpEntry("Medir", "Solicita una medición del módulo de laboratorio activo.", "Interpreta los resultados según la etiqueta y el estado; no todos son mediciones acústicas físicas."),
        ControlHelpEntry("Modo automático del laboratorio", "Permite al laboratorio ejecutar su política automática cuando está disponible.", "Desactívalo para controlar manualmente los pasos de una prueba."),
        ControlHelpEntry("Guardar EVO / Restaurar EVO", "Guarda o recupera el estado evolutivo cuando el puente nativo confirma la operación.", "Verifica el mensaje de resultado; si el motor no está cargado, no se ha guardado nada."),
        ControlHelpEntry("Ejes de supremacía", "Abre los parámetros avanzados de reconstrucción, percepción y sincronización.", "Son ajustes experimentales avanzados: cambia un eje por vez y conserva un perfil conocido.")
    )),
    ControlHelpGroup("ADAPTIVE · DECISIONES Y AUTOMATIZACIÓN", listOf(
        ControlHelpEntry("Intensidad adaptativa", "Limita la fuerza con que la política adaptativa modifica los parámetros compatibles.", "Empieza bajo; si percibes bombeo, cambios de timbre o escena inestable, reduce la intensidad."),
        ControlHelpEntry("Perfil/contexto", "Selecciona el contexto de escucha que guía decisiones adaptativas disponibles.", "Comprueba que el contexto seleccionado coincida con el uso real y compara contra modo manual."),
        ControlHelpEntry("Estado / confianza / telemetría", "Presenta señales internas y disponibilidad de la decisión adaptativa.", "La confianza del modelo no es una medida objetiva de calidad sonora ni sustituye una escucha crítica.")
    )),
    ControlHelpGroup("SPATIAL · HRTF Y GEOMETRÍA", listOf(
        ControlHelpEntry("Sujeto HRTF", "Selecciona el conjunto de datos de cabeza y oído que se usa en el renderizado binaural.", "Prueba perfiles con voces centradas y movimientos conocidos; si no hay confirmación de carga, el cambio no debe darse por aplicado."),
        ControlHelpEntry("Latentes / parámetros HRTF", "Ajustan dimensiones del modelo HRTF cuando el panel y el conjunto seleccionado las admiten.", "Son parámetros avanzados; guarda una referencia y evita mover muchos a la vez."),
        ControlHelpEntry("Distancia / elevación / azimut", "Controlan la ubicación virtual del sonido respecto al oyente.", "Ajusta una coordenada a la vez y escucha si la posición cambia de forma coherente."),
        ControlHelpEntry("Vista de sala / reflexiones", "Visualiza geometría o estimaciones acústicas disponibles.", "Una visualización estimada no equivale a una medición física de cada reflexión.")
    )),
    ControlHelpGroup("PROFILER · ESTADO Y DIAGNÓSTICO", listOf(
        ControlHelpEntry("Beacon de efecto global", "Muestra si hay una fuente de cadencia/correlación válida para observar el efecto global.", "«No disponible» significa que falta evidencia correlacionada; no demuestra que el efecto esté activo."),
        ControlHelpEntry("Reloj hardware / tasa / deriva", "Solo informa del reloj físico cuando existe un productor de muestras PCM validado.", "No confundas el reloj de AudioTrack con el reloj del DAC. Sin productor validado, no se debe afirmar sincronización hardware."),
        ControlHelpEntry("Core ASRC software", "Informa del servo de corrección de deriva sobre el remuestreador software, si la ruta está activa.", "Una corrección software no controla necesariamente el oscilador físico del DAC."),
        ControlHelpEntry("Reloj AudioTrack", "Muestra la cadencia/posición del AudioTrack propio de la app.", "Describe esa ruta de reproducción, no todas las aplicaciones ni el reloj físico del DAC USB."),
        ControlHelpEntry("CPU, memoria, latencia y underruns", "Muestra contadores o mediciones disponibles de coste y salud del procesamiento.", "Compara en las mismas condiciones; un valor ausente no debe interpretarse como cero."),
        ControlHelpEntry("Ruta de salida", "Identifica el dispositivo que Android informa como ruta efectiva de reproducción.", "Conecta el DAC antes de reproducir y confirma el dispositivo enrutado; solicitar USB no garantiza que Android lo haya seleccionado.")
    )),
    ControlHelpGroup("SALIDA · CÓMO ELEGIR EL MODO", listOf(
        ControlHelpEntry("Audífonos con cable", "Permiten probar EQ, HRTF y espacialidad binaural.", "Empieza con perfil neutro y HRTF moderado; compara siempre con el procesamiento apagado."),
        ControlHelpEntry("Audífonos Bluetooth", "La señal pasa por el códec y el procesamiento del dispositivo Bluetooth.", "Comprueba la ruta real; la latencia y el códec dependen del teléfono y los audífonos. No se promete control de su reloj interno."),
        ControlHelpEntry("Estéreo por AUX", "Envía la señal por la salida analógica/cable compatible hacia un amplificador o estéreo.", "Desactiva efectos de campo duplicados en el receptor y comienza con niveles bajos para evitar saturación."),
        ControlHelpEntry("DAC USB-C", "Permite reproducir mediante un DAC USB reconocido por Android.", "Verifica que el dispositivo figure como ruta efectiva. La app puede leer el enrutamiento, pero eso no prueba por sí solo la tasa física ni el control del reloj del DAC."),
        ControlHelpEntry("Sistema multicanal", "Puede aprovechar perfiles y geometría multicanal cuando la ruta física y el motor los soportan.", "Usa el mapa de canales del receptor y valida cada altavoz; WFS virtual no reemplaza un arreglo físico.")
    ))
)

@Composable
// Fase 2: startExpanded = true muestra la guía completa al entrar (la
// pantalla Aprende la abre así); por defecto sigue colapsada como antes.
fun ControlHelpCatalogPanel(startExpanded: Boolean = false) {
    var expanded by remember { mutableStateOf(startExpanded) }
    Column(verticalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.fillMaxWidth()) {
        Text("GUÍA EN ESPAÑOL · TODOS LOS CONTROLES", style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.primary)
        Text("Qué hace cada ajuste, cuándo conviene usarlo y qué límites tiene. Los nombres pueden variar según el módulo y la ruta disponibles.",
            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        OutlinedButton(onClick = { expanded = !expanded }, modifier = Modifier.fillMaxWidth()) {
            Text(if (expanded) "OCULTAR GUÍA COMPLETA" else "EXPLICAR CONTROLES Y RECOMENDACIONES")
        }
        if (expanded) {
            controlHelpGroups.forEach { group ->
                Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface)) {
                    Column(Modifier.fillMaxWidth().padding(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                        Text(group.title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.primary)
                        group.entries.forEach { entry ->
                            Column(verticalArrangement = Arrangement.spacedBy(3.dp)) {
                                Text(entry.name, style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.SemiBold)
                                Text("Qué hace: ${entry.purpose}", style = MaterialTheme.typography.bodySmall)
                                Text("Cómo usarlo: ${entry.use}", style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun ControlHelpText(label: String) {
    val key = label.lowercase()
        .replace("á", "a").replace("é", "e").replace("í", "i")
        .replace("ó", "o").replace("ú", "u").replace("ü", "u")
    val explanation = when {
        ("exciter" in key || "armonic" in key) && !("ganancia armonica" in key || "transarmonic" in key || "harmonic gain" in key) -> "Qué hace: añade armónicos para aumentar presencia y brillo. Cómo usarlo: sube gradualmente; si las voces o los platillos se vuelven ásperos, reduce el valor."
        "eq gain" in key || key == "eq" || "ecual" in key -> "Qué hace: realza o atenúa el nivel tonal de la etapa indicada. Cómo usarlo: corrige de forma moderada y evita grandes realces que puedan provocar saturación."
        "stereo width" in key || "anchura" in key || "width" in key || "spread" in key -> "Qué hace: cambia la amplitud estéreo o espacial percibida. Cómo usarlo: aumenta poco a poco y comprueba que las voces sigan centradas y que el sonido mono no se vacíe."
        // v2.5.448: el umbral de COMPRESOR no debe tragarse el umbral de
        // membrana LIF SNN ("UMBRAL DE POTENCIAL DE MEMBRANA LIF SNN") — esa
        // etiqueta contiene "umbral" y caía aquí antes de llegar a la entrada
        // de LIF. La guarda excluye LIF/membrana/SNN.
        ("threshold" in key || "umbral" in key) && !("lif" in key || "membrana" in key || "snn" in key) -> "Qué hace: fija el nivel a partir del cual empieza a actuar el compresor. Cómo usarlo: un umbral más bajo suele hacer que la compresión actúe más a menudo."
        "ratio" in key || "relacion" in key -> "Qué hace: determina cuánto se reducen los niveles que superan el umbral. Cómo usarlo: usa relaciones suaves para música; si pierde pegada, reduce la compresión."
        "coherencia de fase" in key -> "Qué hace: regula el procesamiento de coherencia/fase de la etapa correspondiente. Cómo usarlo: empieza bajo y compara voces, transitorios y compatibilidad mono."
        "cancelacion objetos atmos" in key || "antidolby" in key -> "Qué hace: ajusta la intensidad de la etapa de procesamiento atmosférico indicada. Cómo usarlo: compara activado y desactivado al mismo volumen; si aparecen artefactos, baja la intensidad."
        "supresion coherencia falsa" in key -> "Qué hace: regula la mezcla espacial húmeda de la etapa asociada. Cómo usarlo: reduce si el centro vocal se vuelve difuso o la imagen pierde estabilidad."
        "ring buffer" in key || "spsc" in key -> "Qué hace: ajusta un factor interno del búfer circular de comunicación DSP. Cómo usarlo: es avanzado; deja el valor recomendado salvo que estés midiendo estabilidad, latencia o pérdidas."
        "tinyml" in key || "inferencia" in key -> "Qué hace: ajusta la ganancia usada por la etapa de inferencia TinyML compatible. Cómo usarlo: modifica poco a poco y vigila artefactos; más ganancia no implica más calidad."
        "intensidad de reconstruccion" in key -> "Qué hace: determina cuánto interviene la reconstrucción perceptual del evento. Cómo usarlo: comienza bajo y compara con la reconstrucción desactivada."
        // v2.5.484: controles de Física Nueva Ω y v3 — cada slider de las
        // dos tarjetas pasa su clave aquí. Estas ramas van ANTES de las
        // genéricas ("distancia", "frecuencia", "ganancia", "volterra") para
        // que el texto específico gane al genérico.
        "mezcla maestra" in key -> "Qué hace: controla cuánto del motor de física nueva se mezcla con la señal (1.0 = 100%, hasta 1.5 = overdrive). Cómo usarlo: déjalo en 1.0; el overdrive es solo para pruebas."
        "lgn" in key -> "Qué hace: mezcla el modelo de pabellón auricular que coloca el sonido en 3D, fuera de la cabeza. Cómo usarlo: en 1.0 el efecto es pleno; bájalo si la imagen se siente exagerada."
        "bright-spot" in key -> "Qué hace: amortigua el punto brillante donde la HRTF concentra energía y suena áspero. Cómo usarlo: súbelo si notas aspereza en agudos; bájalo si el sonido pierde aire."
        "eco torso" in key -> "Qué hace: añade el retardo del torso, que da cuerpo a la localización. Cómo usarlo: déjalo bajo; subirlo demasiado ensucia la imagen."
        "tope de refuerzo" in key -> "Qué hace: limita cuánto puede reforzar el motor (default 3.0 dB). Cómo usarlo: 3 dB es el tope seguro; subirlo da más presencia pero arriesga picos."
        "compensacion de aire" in key -> "Qué hace: compensa la absorción de agudos del aire a distancia. Cómo usarlo: en sala pequeña déjalo en 0; súbelo solo a distancias grandes."
        "radio cabeza" in key -> "Qué hace: ajusta el radio de cabeza que usa el modelo para el tiempo entre oídos (default 8.75 cm). Cómo usarlo: ajústalo solo si mediste tu cabeza."
        "firma elite" in key -> "Qué hace: mezcla la curva de referencia élite Harman 2018. Cómo usarlo: en 0 el motor no la aplica; súbelo si quieres el tinte de la firma élite."
        "diafonia" in key -> "Qué hace: cancela la diafonía (el sonido de la bocina izquierda que llega al oído derecho) antes de que ocurra. Cómo usarlo: 1.0 en bocinas; con audífonos no hay diafonía que cancelar."
        "sigma distancia" in key -> "Qué hace: fija la distancia bocinas→cabeza que usa el modelo (default 3.5 m, la distancia real al Genezi). Cómo usarlo: pon la distancia real; un valor absurdo (15.79 m) causaba el piiiii."
        "spl estimado" in key -> "Qué hace: fija el nivel de escucha que el motor asume para la no-linealidad (default 85 dB). Cómo usarlo: pon tu nivel real de escucha."
        "volterra laguerre" in key -> "Qué hace: pre-distorsión que cancela la distorsión H2 del Genezi (kernel Laguerre, polo 0.9983). Cómo usarlo: 1.0 con el Genezi; 0 si no hay kernel."
        "lazo variacional" in key -> "Qué hace: lazo de energía libre que ajusta MOC y onsets según la señal. Cómo usarlo: experimental; déjalo en 0 salvo pruebas controladas."
        "sub-graves" in key && "frecuencia" in key -> "Qué hace: fija la frecuencia del modo de graves que corta esta etapa. Cómo usarlo: no la muevas salvo que hayas medido tu sala."
        "sub-graves" in key && "ganancia" in key -> "Qué hace: fija cuánto corta (negativo) esta etapa de graves. Cómo usarlo: déjala en negativo; nunca la subas a positivo sin medir (Fase 4 es cut-only)."
        "sub-graves" in key && " q" in key -> "Qué hace: fija qué tan estrecho es el corte de esta etapa (Q alto = solo el pico, Q bajo = más ancho). Cómo usarlo: Q 3–4 es el default."
        "sub-graves" in key -> "Qué hace: controla una etapa de sub-graves modal (solo corta los picos de la sala, nunca refuerza). Cómo usarlo: déjala activa con su default; apágala solo si sabes que ese modo no existe en tu sala."
        "mezcla cola" in key -> "Qué hace: regula cuánta cola de sala real (RIR medida) se mezcla con el directo (default 0.5). Cómo usarlo: bájala si la grabación ya tiene reverberación."
        "mezcla h3" in key -> "Qué hace: regula la corrección del armónico 3 del Genezi (Klippel, default 1.0). Cómo usarlo: 1.0 con el Genezi; 0 = sin corrección H3."
        "equipo omega" in key -> "Qué hace: dice al motor qué equipo suena (audífonos, bocina G85, BT, Genezi, auto, PA/club, estadio). Cómo usarlo: elige el equipo real; Genezi = E4."
        "abx" in key -> "Qué hace: prueba ciega Off → A (bypass) → B (física nueva) → Off. Cómo usarlo: compara sin sesgo al mismo volumen."
        "interruptor maestro" in key -> "Qué hace: enciende o apaga toda la física nueva; en 0 el motor hace bypass bit-exacto. Cómo usarlo: apágalo para comparar con y sin física nueva al mismo volumen."
        "radio cefalico" in key || "head radius" in key -> "Qué hace: ajusta el radio de cabeza asumido por el modelo auditivo. Cómo usarlo: cambia solo si quieres personalizar la geometría; no es un control de volumen."
        "hoa" in key || "inmersividad" in key -> "Qué hace: regula cuánto se expande el contenido hacia el campo espacial HOA. Cómo usarlo: compara con estéreo original y reduce si el centro pierde precisión."
        "cruce de graves" in key || "xover" in key || "crossover" in key -> "Qué hace: fija la frecuencia de cruce de la etapa de graves. Cómo usarlo: en AUX/amplificador evita duplicar el crossover del receptor; usa el mismo ajuste en todo el sistema."
        "coherencia de graves" in key -> "Qué hace: regula la coherencia de la etapa de graves para la salida compatible. Cómo usarlo: comienza en el valor neutro y evita ajustes extremos si el grave pierde definición."
        "profundidad temporal" in key || "profundidad maxima" in key -> "Qué hace: limita la cantidad de modulación espacial que sigue la música. Cómo usarlo: empieza con poca profundidad y apágalo si la escena se mueve de forma artificial."
        "fuerza de vibracion" in key || "haptic" in key || "habtica" in key -> "Qué hace: regula la intensidad de vibración en hardware compatible. Cómo usarlo: ajusta a un nivel cómodo; la vibración no aumenta el grave acústico que llega al oído."
        "retumbo" in key || "rumble" in key -> "Qué hace: controla la componente de vibración grave sostenida. Cómo usarlo: mantén un nivel moderado para evitar vibración continua molesta."
        "sincronia" in key || "trim" in key || "retardo" in key || "delay" in key -> "Qué hace: desplaza la sincronización temporal de la etapa indicada. Cómo usarlo: cambia en pasos pequeños; si el efecto queda adelantado o retrasado, vuelve al valor anterior."
        "target agc" in key || "agc target" in key || "lufs" in key || "loudness" in key || "sonoridad" in key -> "Qué hace: define la referencia de nivel para la compensación automática de sonoridad. Cómo usarlo: elige un objetivo moderado y evita perseguir más volumen a costa de dinámica."
        "azimut" in key || "azimuth" in key -> "Qué hace: coloca la fuente virtual a izquierda o derecha alrededor del oyente. Cómo usarlo: deja 0° para el centro y usa cambios graduales para posicionar una fuente."
        "elevacion" in key || "elevation" in key || "posicion y" in key -> "Qué hace: sitúa la fuente virtual por encima o debajo del plano del oyente. Cómo usarlo: ajusta según la posición deseada; una HRTF incompatible puede reducir la precisión."
        "profundidad" in key || "distance" in key || "distancia" in key || "posicion z" in key -> "Qué hace: modifica la profundidad o distancia virtual. Cómo usarlo: cambia poco a poco y conserva un punto de referencia."
        "posicion x" in key || "oyente x" in key || "posicion lateral" in key -> "Qué hace: sitúa al oyente a lo ancho de la sala virtual. Cómo usarlo: mide aproximadamente desde una pared de referencia."
        "oyente y" in key || "altura" in key || "elevacion" in key -> "Qué hace: representa la altura del oyente o la fuente en la geometría virtual. Cómo usarlo: toma como referencia el suelo y la altura de tus oídos."
        "generacion" in key -> "Qué hace: define cuántas rondas de evolución ejecuta el laboratorio. Cómo usarlo: aumenta solo en pruebas controladas; más rondas consumen más tiempo."
        "poblacion" in key -> "Qué hace: define el número de soluciones candidatas del proceso evolutivo. Cómo usarlo: valores mayores pueden exigir más CPU y tiempo; no garantizan mejor sonido."
        "intensidad" in key -> "Qué hace: regula la fuerza del efecto o de la adaptación indicada. Cómo usarlo: empieza bajo, sube gradualmente y detente si notas bombeo, aspereza o fatiga."
        ("gain" in key || "ganancia" in key) && !("armonica" in key || "master" in key || "nho" in key || "npe" in key) -> "Qué hace: ajusta el nivel de la etapa indicada. Cómo usarlo: evita picos y clipping; más ganancia no equivale a mayor fidelidad."
        "frecuencia" in key || "frequency" in key -> "Qué hace: selecciona la frecuencia de operación indicada por este módulo. Cómo usarlo: respeta los límites y la compatibilidad de la salida conectada."
        "mezcla binaural" in key || "binaural wet" in key -> "Qué hace: determina cuánto procesamiento binaural se mezcla con la señal. Cómo usarlo: empieza con una mezcla moderada y baja si la escena suena artificial."
        "sala acustica" in key || "room size" in key -> "Qué hace: cambia el tamaño/intensidad del espacio simulado por la convolución. Cómo usarlo: usa ajustes bajos para grabaciones ya reverberantes."
        "sujeto" in key && "hrtf" in key -> "Qué hace: selecciona el perfil de cabeza y oído usado para la localización binaural. Cómo usarlo: compara con voces centradas; el cambio solo cuenta si el conjunto se carga correctamente."
        "manual" in key -> "Qué hace: mantiene los parámetros compatibles bajo control directo y evita cambios adaptativos. Cómo usarlo: actívalo para pruebas A/B repetibles."
        "adaptativo" in key || "adaptive" in key -> "Qué hace: permite que el motor ajuste parámetros compatibles según el contexto disponible. Cómo usarlo: desactívalo si quieres un timbre constante y fácil de comparar."
        "hrtf" in key || "binaural" in key -> "Qué hace: procesa la señal para simular posición y dirección mediante funciones de cabeza y oído. Cómo usarlo: suele ser más útil con audífonos; compara con el efecto apagado."
        "orbita" in key || "movimiento 3d" in key -> "Qué hace: mueve lentamente la escena virtual en el espacio. Cómo usarlo: es un efecto creativo; apágalo si necesitas una imagen sonora estable."
        "convolucion" in key || "rir" in key -> "Qué hace: aplica una respuesta impulsional para simular características de un espacio. Cómo usarlo: reduce la mezcla si el audio ya contiene mucha reverberación."
        "posicion" in key || "oyente" in key -> "Qué hace: cambia la posición del oyente o de la fuente dentro de la geometría virtual. Cómo usarlo: usa medidas aproximadas de la sala y cambia una coordenada por vez."
        "compresor" in key -> "Qué hace: controla el rango dinámico para reducir picos o acercar niveles. Cómo usarlo: usa ajustes moderados y reduce si la música pierde pegada."
        "reset" in key || "reiniciar" in key -> "Qué hace: devuelve el módulo o la escena a sus valores iniciales. Cómo usarlo: guarda antes los ajustes que quieras conservar."
        "medir" in key || "medicion" in key -> "Qué hace: inicia una medición del módulo indicado. Cómo usarlo: sigue las condiciones que muestra la pantalla y no interpretes datos ausentes como cero."
        "guardar" in key || "restaurar" in key -> "Qué hace: guarda o recupera parámetros del módulo cuando la operación está disponible. Cómo usarlo: confirma el resultado antes de asumir que los cambios se conservaron."
        "impacto" in key || "punch" in key -> "Qué hace: aumenta o reduce la sensación de golpe de los transitorios. Cómo usarlo: empieza con poco; demasiado impacto puede endurecer bombos y percusión."
        "apertura wfs" in key || "wfs" in key -> "Qué hace: ajusta la apertura del campo de altavoces virtuales WFS. Cómo usarlo: tiene sentido en una ruta/arreglo compatible; no sustituye altavoces físicos ni garantiza WFS en audífonos."
        "ganancia armonica" in key || "transarmonic" in key || "harmonic gain" in key -> "Qué hace: regula la cantidad de armónicos añadidos por la etapa indicada. Cómo usarlo: sube gradualmente y vigila aspereza, sibilancia y clipping."
        "inhibicion lateral" in key -> "Qué hace: controla la inhibición entre canales o bandas de la etapa neuromórfica. Cómo usarlo: modifica poco a poco y compara la separación sin perder cuerpo."
        "compresion ohc" in key -> "Qué hace: ajusta la compresión del modelo de células ciliadas externas (OHC). Cómo usarlo: es un parámetro avanzado del modelo auditivo; cambia uno por vez y compara con la referencia."
        "agc rate" in key || "tasa agc" in key -> "Qué hace: regula la rapidez de la compensación automática de ganancia. Cómo usarlo: valores rápidos pueden producir bombeo; reduce la tasa si el volumen fluctúa."
        "master gain" in key || "ganancia master" in key || "npe master" in key -> "Qué hace: ajusta la ganancia general de la etapa indicada. Cómo usarlo: evita clipping y compara a volumen percibido igualado; más volumen no significa mayor fidelidad."
        "attack" in key || "ataque" in key -> "Qué hace: determina cuánto tarda el compresor en reaccionar al superar el umbral. Cómo usarlo: ataque corto controla picos; uno más largo conserva más transitorio."
        "release" in key || "liberacion" in key -> "Qué hace: determina cuánto tarda el compresor en dejar de actuar tras bajar la señal. Cómo usarlo: ajusta al ritmo para evitar bombeo o pérdida de dinámica."
        "tasa de mutacion" in key || "mutation" in key -> "Qué hace: controla cuánto varían las soluciones candidatas entre generaciones. Cómo usarlo: es un ajuste de laboratorio; cambia poco a poco y registra el resultado."
        "volterra" in key || "imd" in key || "no-lineal" in key -> "Qué hace: ajusta una etapa experimental de compensación no lineal o distorsión de intermodulación. Cómo usarlo: déjalo en el valor neutro si no tienes una señal de prueba y medición reproducible."
        "bark" in key || "deformacion alopaso" in key -> "Qué hace: ajusta la deformación del modelo en escala perceptual Bark. Cómo usarlo: es experimental; cambia un parámetro por vez y compara con bypass."
        "cinta" in key || "jiles-atherton" in key || "histeresis" in key -> "Qué hace: modela una característica no lineal asociada a cinta magnética. Cómo usarlo: es un efecto de coloración, no una corrección transparente; mantenlo bajo si buscas fidelidad."
        "potencial de membrana" in key || "lif snn" in key -> "Qué hace: define el umbral de disparo de la etapa de red neuronal con neuronas LIF. Cómo usarlo: parámetro experimental; modifica solo durante pruebas controladas."
        "fir personalizado" in key || "pinna manifold" in key -> "Qué hace: mezcla la salida filtrada FIR personalizada con la señal original. Cómo usarlo: empieza en mezcla baja y verifica que el timbre no se vuelva hueco."
        "concha" in key || "cavum" in key || "helix" in key || "anti-helix" in key || "latente" in key -> "Qué hace: ajusta una dimensión geométrica del modelo de pabellón auricular/HRTF. Cómo usarlo: es personalización avanzada; cambia una dimensión por vez y guarda una referencia."
        "ancho interaural" in key || "head width" in key -> "Qué hace: ajusta el ancho cefálico asumido por el modelo binaural. Cómo usarlo: modifica solo para personalizar la geometría y comprueba la estabilidad de la imagen."
        "mso" in key || "farrow" in key || "interaural" in key -> "Qué hace: ajusta la alineación temporal/fase entre ambos oídos. Cómo usarlo: valores extremos pueden mover o difuminar el centro; cambia en pasos pequeños."
        "ángulo espacial" in key || "angulo espacial" in key -> "Qué hace: gira la dirección de la fuente virtual. Cómo usarlo: usa movimientos graduales y vuelve al centro si la localización se vuelve confusa."
        "ancho espacial" in key || "spatial control" in key -> "Qué hace: controla la extensión espacial percibida. Cómo usarlo: mantén moderación si la voz central pierde estabilidad."
        "nho" in key || "npe" in key -> "Qué hace: ajusta la intensidad de la etapa armónica/neuromórfica indicada. Cómo usarlo: es procesamiento avanzado; empieza bajo y vigila fatiga y distorsión."
        "temperatura" in key || "thermal" in key -> "Qué hace: ajusta o muestra el comportamiento térmico del motor. Cómo usarlo: no fuerces límites para ganar volumen; deja que la protección térmica actúe."
        // v2.5.448: tres controles del Motor Híbrido (BINAURAL) caían al
        // texto genérico del `else` porque el catálogo no tenía entrada para
        // ellos. Ahora cada uno explica lo que hace de verdad.
        "absorcion" in key || "absorción" in key || "absorption" in key -> "Qué hace: controla cuánta energía absorben las paredes virtuales de la sala simulada (más absorción = sala más seca, menos cola). Cómo usarlo: sube la absorción si la sala suena embarrada o con demasiada cola; bájala si suena muerta o sin aire."
        "amortiguamiento" in key || "dampening" in key || "damp" in key -> "Qué hace: atenúa los agudos con la distancia dentro de la sala simulada (amortiguamiento del aire HF). Cómo usarlo: sube el amortiguamiento si los agudos suenan ásperos o metálicos a distancia; bájalo si la sala pierde brillo."
        "reverb" in key || "schroeder" in key -> "Qué hace: regula cuánta reverberación Schroeder/Moorer se mezcla con la señal seca (wet de la sala híbrida). Cómo usarlo: empieza bajo; sube solo si la grabación suena seca y la sala la envuelve sin embarrar la voz."
        else -> "Qué hace: ajusta el parámetro indicado por este módulo. Cómo usarlo: cambia una cosa por vez, compara con la referencia al mismo volumen y vuelve al ajuste anterior si empeora la claridad."
    }
    Text(explanation, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
}
