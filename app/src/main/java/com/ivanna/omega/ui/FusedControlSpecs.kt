package com.ivanna.omega.ui

/**
 * FusedControlSpecs — etiquetas de los 46 controles únicos (§6, v2.5.473).
 *
 * Fuente: PROPOSER §6 (tabla qué es / para qué sirve / cómo se usa) y
 * `ControlHelpCatalog.kt` (8 grupos, tono honesto) donde la fila lo marca
 * con [catálogo]. Cada entrada es un [SpecInfo] (qué es / para qué sirve /
 * cuándo usarlo / cuándo NO usarlo) y se muestra con el botón "i"
 * ([SpecInfoButton] en tarjetas, [SpecInfoButtonSmall] junto a sliders).
 * Nada vago: cada ficha dice realmente qué es, para qué sirve y cuándo
 * sí y cuándo no. Nada promete lo no medido.
 */
object FusedControlSpecs {
    val profileBand = SpecInfo(
        title = "Perfil de banda",
        what = "Un conjunto de ajustes pensado para una banda y un tema de referencia.",
        purpose = "Carga de una vez EQ, campo, Φ, Volterra y neuro para esa música.",
        whenUse = "Elige en Perfiles; compara un perfil cada vez y vuelve a Steve Miller si notas distorsión o fatiga.",
        whenAvoid = "No mezcles dos perfiles a mano: un perfil es un todo, no una lista de piezas."
    )
    val masterVolume = SpecInfo(
        title = "Volumen maestro",
        what = "Nivel de salida de la cadena DSP (0 dB = neutro).",
        purpose = "Sube o baja todo sin tocar la curva ni el campo.",
        whenUse = "Desliza; 0 dB es el neutro. Si algo distorsiona, baja aquí antes de tocar el EQ.",
        whenAvoid = "No lo uses como sustituto del EQ: subirlo todo no arregla un balance roto."
    )
    val outputMode = SpecInfo(
        title = "Salida: Audífonos / Genezi",
        what = "Qué está conectado a la salida: audífonos o el Genezi por DAC USB.",
        purpose = "La puerta de bocinas adapta la cadena: en Genezi deja pasar lo binaural (ORBE/HOLOS, anchura, armónicos, AntiDolby) y pone a neutro lo general (compresor, loudness, exciter). Tercer modo GENEZI, v2.5.483.",
        whenUse = "Tócalo al cambiar de salida; el cambio es inmediato y se guarda.",
        whenAvoid = "No lo dejes en Genezi con audífonos puestos: la cadena es de bocinas (con diafonía) y la escena se oye mal."
    )
    val systemCapture = SpecInfo(
        title = "Captura del sistema",
        what = "Escucha lo que suena en otras apps (Qobuz/Tidal en navegador) por la Ruta A.",
        purpose = "Sin ella solo suena la música local de IVANNA.",
        whenUse = "Actívala solo cuando vayas a escuchar fuera de la app; te pedirá el permiso de captura.",
        whenAvoid = "No la dejes encendida siempre: captura todo lo que suena en el teléfono."
    )
    val orbeAmount = SpecInfo(
        title = "ORBE · Cantidad",
        what = "Intensidad del campo cercano esférico (0 = apagado, 1 = pleno).",
        purpose = "Abre la escena en 3D alrededor de la cabeza.",
        whenUse = "1.0 es la referencia; en Genezi también pasa (modo GENEZI, v2.5.483).",
        whenAvoid = "No la subas para 'más efecto' en una mezcla ya espacial: se desarma la imagen."
    )
    val orbeAzimuth = SpecInfo(
        title = "ORBE · Azimut",
        what = "Dirección horizontal de la escena (−180°..180°).",
        purpose = "Gira el escenario a izquierda o derecha.",
        whenUse = "Déjalo en 0° (centrado) salvo que la mezcla te pida girarla.",
        whenAvoid = "No lo uses para centrar una voz descentrada: eso es paneo, no azimut de escena."
    )
    val orbeDistance = SpecInfo(
        title = "ORBE · Distancia",
        what = "A qué distancia virtual está la fuente (0.15–2.0 m).",
        purpose = "Acerca o aleja el sonido: campo cercano íntimo o sala amplia.",
        whenUse = "0.35 m es la referencia de Steve Miller; alarga solo si la mezcla se siente encima.",
        whenAvoid = "No la alejes para 'dar aire': a 2 m la escena pierde el cuerpo del campo cercano."
    )
    val orbeWidth = SpecInfo(
        title = "ORBE · Ancho",
        what = "Apertura del campo (1 = neutro).",
        purpose = "Ensancha o estrecha la escena sin tocar el balance.",
        whenUse = "1.0 neutro; sube poco a poco y conserva la voz al centro.",
        whenAvoid = "No lo subas con la voz al centro en riesgo: el centro se vacía."
    )
    val orbeHrtfWet = SpecInfo(
        title = "ORBE · Wet HRTF",
        what = "Cuánta HRTF (cabeza/oído) se mezcla (0 = seco, 1 = pleno).",
        purpose = "Coloca los sonidos en 3D con audífonos.",
        whenUse = "1.0 es la referencia; baja si la imagen se vuelve difusa.",
        whenAvoid = "Con bocinas no hace nada útil: la HRTF es para audífonos."
    )
    val orbeHeadTracking = SpecInfo(
        title = "ORBE · Seguimiento de cabeza",
        what = "La escena sigue (o no) el giro de tu cabeza.",
        purpose = "Con head-tracking el sonido queda fijo en la sala al girar.",
        whenUse = "Apagado por defecto: actívalo solo tras medir tu cabeza (\"Medir cabeza\").",
        whenAvoid = "No lo actives sin medida de cabeza: la escena se mueve con un radio inventado."
    )
    val orbeMeasureHead = SpecInfo(
        title = "ORBE · Medir cabeza",
        what = "Foto + tarjeta para medir el radio de tu cabeza.",
        purpose = "La HRTF y el parallax usan tu medida real, no un promedio.",
        whenUse = "Hazla una vez, con buena luz; el valor queda guardado.",
        whenAvoid = "No la repitas a cada rato: la medida no cambia, y una mala foto empeora el radio."
    )
    val orbeXtc = SpecInfo(
        title = "ORBE · XTC",
        what = "Cancelación de diafonía entre altavoces.",
        purpose = "En bocinas evita que el canal izquierdo llegue al oído derecho (y viceversa).",
        whenUse = "En audífonos no hace nada (se fuerza OFF); en Genezi se enciende sola.",
        whenAvoid = "No la fuerces con audífonos: no hay diafonía que cancelar."
    )
    val orbeUpmix = SpecInfo(
        title = "ORBE · Upmixer HOA / Inmersividad",
        what = "Reparte el estéreo en un campo espacial de orden superior.",
        purpose = "Experimentar con espacialidad más allá del estéreo.",
        whenUse = "Para una mezcla estéreo fiel, compáralo apagado; es experimental.",
        whenAvoid = "No lo dejes encendido por defecto: cambia la mezcla que el ingeniero entregó."
    )
    val orbeWfs = SpecInfo(
        title = "ORBE · Apertura WFS",
        what = "Síntesis de frente de onda para un arreglo virtual de altavoces.",
        purpose = "Crea un frente de onda en la habitación (con arreglo compatible).",
        whenUse = "0 = apagado. Con audífonos no crea un frente físico.",
        whenAvoid = "No esperes un frente físico con audífonos: el WFS necesita altavoces."
    )
    val orbeParallax = SpecInfo(
        title = "ORBE · Parallax",
        what = "Cuánto mueve el head-tracking la escena (0 = nada, 1 = pleno).",
        purpose = "Ajusta la respuesta al giro de cabeza sin tocar la cantidad de ORBE.",
        whenUse = "1.0 es la referencia; baja si la escena se mueve demasiado.",
        whenAvoid = "No lo bajes a 0 con el tracking encendido: el giro dejaría de responder."
    )
    val holosAmount = SpecInfo(
        title = "HOLOS · Cantidad",
        what = "Intensidad del campo acústico unificado (0 = apagado, 0.5 = referencia).",
        purpose = "Da cuerpo y coherencia de sala a toda la mezcla.",
        whenUse = "0.5 es la referencia; en Genezi también pasa (modo GENEZI, v2.5.483).",
        whenAvoid = "No lo subas para 'más cuerpo': pasado 0.5 el campo se come los transitorios."
    )
    val phiAmount = SpecInfo(
        title = "Φ · Cantidad",
        what = "Intensidad del Hiper-Operador de Estado (0 = apagado, 0.5 = referencia).",
        purpose = "Bucle cerrado neural: pre-distorsión y sensado que afinan el sonido muestra a muestra.",
        whenUse = "0.5 es la referencia; es el operador que SÍ suena en Genezi.",
        whenAvoid = "No lo apagues para comparar 'con y sin': Φ es el corazón de la cadena, no un adorno."
    )
    val phiPreset = SpecInfo(
        title = "Φ · Preset",
        what = "Para qué salida está afinado Φ: Teléfono / Sala / Auto.",
        purpose = "Cada entorno tiene su curva y su geometría.",
        whenUse = "Teléfono con audífonos, Sala con el Genezi; no lo cambies a mano si ya elegiste la salida.",
        whenAvoid = "No pongas 'Sala' con audífonos: la geometría es de bocinas, no de cabeza."
    )
    val holosHarmonics = SpecInfo(
        title = "HOLOS · Armónicos",
        what = "Armónicos del campo (0–0.002, rango validado).",
        purpose = "Aire y brillo del campo sin usar el exciter genérico.",
        whenUse = "0.0012 es la referencia; no subas del máximo: fuera de rango no está validado.",
        whenAvoid = "Nunca subas del máximo 0.002: fuera de rango no está validado y el campo se endurece."
    )
    val holosCoherence = SpecInfo(
        title = "HOLOS · Coherencia",
        what = "Cuánto se mantiene unido el campo (0–1).",
        purpose = "Evita que la escena se desarme en pasajes complejos.",
        whenUse = "0.8 es la referencia; baja si el campo suena rígido.",
        whenAvoid = "No la bajes para 'soltar' el campo: la escena se desarma en los pasajes densos."
    )
    val holosGamma = SpecInfo(
        title = "HOLOS · Campo Γ",
        what = "Fuerza del campo Γ, Ley 1 (0–2, 1 = neutro).",
        purpose = "Escala global del campo unificado.",
        whenUse = "Déjalo en 1.0 salvo que sepas qué estás midiendo.",
        whenAvoid = "No lo toques por curiosidad: es la escala global del campo, no un tono."
    )
    val holosWarmth = SpecInfo(
        title = "HOLOS · Calidez",
        what = "Calidez del campo (0–1).",
        purpose = "Redondea el campo hacia lo analógico.",
        whenUse = "0.5 es la referencia del perfil.",
        whenAvoid = "No la subas para 'calentar' una mezcla fría: eso lo hace el EQ, no el campo."
    )
    val holosDynamics = SpecInfo(
        title = "HOLOS · Dinámica",
        what = "Dinámica propia del campo (1 = sin compresión extra).",
        purpose = "Deja respirar al campo o lo aprieta.",
        whenUse = "1.0 es la referencia; baja solo si el campo bombea.",
        whenAvoid = "No la bajes para comprimir la mezcla: para eso está el compresor de PULSO."
    )
    val holosRt60 = SpecInfo(
        title = "HOLOS · RT60",
        what = "Reverberación de la sala del campo (0.05–1.5 s).",
        purpose = "Tamaño de la sala virtual que envuelve la mezcla.",
        whenUse = "0.34 s es la referencia; más largo = sala mayor, no mejor.",
        whenAvoid = "No lo alargues para 'más ambiente': una sala mayor no es una sala mejor."
    )
    val holosPumping = SpecInfo(
        title = "HOLOS · Bombeo",
        what = "Bombeo del campo (objetivo 0.0).",
        purpose = "Indica si el campo respira con la música (defecto) o se mantiene firme.",
        whenUse = "Sin control: fijo en 0.0 por diseño. Si oyes bombeo, es otro módulo.",
        whenAvoid = "No busques un control para esto: no existe, es una lectura de diseño."
    )
    val salaRoom = SpecInfo(
        title = "SALA VIVA · RT60 / Tamaño / Wet RIR",
        what = "Reverberación, tamaño y mezcla de la sala del oyente.",
        purpose = "Adapta la sala virtual a donde escuchas.",
        whenUse = "0.34 s / 5.0 m / 0.22 es la referencia; mide tu sala para afinarlo.",
        whenAvoid = "No copies los valores de otra sala: cada sala tiene su RT60 y su tamaño."
    )
    val salaHearing = SpecInfo(
        title = "SALA VIVA · Perfil auditivo",
        what = "Curva de audición: Plana / Harman / Personalizada.",
        purpose = "Compensa tu audición al renderizar la sala.",
        whenUse = "Plana si no tienes medición; Harman es la referencia élite.",
        whenAvoid = "No elijas 'Personalizada' sin una medición real: sería una curva inventada."
    )
    val salaMeasure = SpecInfo(
        title = "SALA VIVA · Medir sala",
        what = "Medición acústica con el micrófono (tonos + corrección).",
        purpose = "Estima la corrección de tu sala y tus bocinas.",
        whenUse = "Hazla en silencio, con el teléfono en el punto de escucha; concede el micrófono solo entonces.",
        whenAvoid = "No la hagas con ruido de fondo o con el teléfono en la mano: la medición sale torcida."
    )
    val salaCorrection = SpecInfo(
        title = "SALA VIVA · Corrección de sala",
        what = "Aplica (o no) la corrección medida.",
        purpose = "Corrige el balance tonal amplio de tu sala (50 Hz–6 kHz).",
        whenUse = "Úsala con altavoces; en audífonos no sustituye una calibración de audífonos.",
        whenAvoid = "No la enciendas sin medición: sin datos no hay nada que corregir."
    )
    val salaSweep = SpecInfo(
        title = "SALA VIVA · Grabar barrido Genezi",
        what = "Reproduce los dos barridos ESS al Genezi y graba el micrófono en WAV.",
        purpose = "Identifica la física real del Genezi (H1/H2/H3) para su kernel Volterra.",
        whenUse = "Con el DAC al Genezi, volumen fijo, sala en silencio; dura ~30 s; guarda genezi_rec_lo/hi.wav en Download.",
        whenAvoid = "No lo grabes con música sonando o el volumen moviéndose: el barrido sale inservible."
    )
    val volterraCorrection = SpecInfo(
        title = "VOLTERRA · Corrección",
        what = "Intensidad del corrector de transductor (0 = apagado, 1 = pleno).",
        purpose = "Linealiza el altavoz/bafle: quita distorsión propia del transductor.",
        whenUse = "1.0 es la referencia; el kernel (PZ1D/Genezi) lo elige la salida, no tú.",
        whenAvoid = "No la bajes para 'oír el altavoz tal cual': el corrector es la linealización, no un efecto."
    )
    val volterraTemporal = SpecInfo(
        title = "VOLTERRA · Dinámica temporal",
        what = "Modulación espacial de los transitorios (0–1).",
        purpose = "Da movimiento a la escena en los ataques.",
        whenUse = "0.0 es la referencia: la escena no debe moverse sola.",
        whenAvoid = "No la subas para 'más vida': la escena empieza a moverse sola en los ataques."
    )
    val volterraEmf = SpecInfo(
        title = "VOLTERRA · Back-EMF",
        what = "Sensado de la fuerza contra-electromotriz del altavoz.",
        purpose = "El corrector lee lo que el altavoz realmente hace.",
        whenUse = "Déjalo encendido; apagarlo es solo para diagnóstico.",
        whenAvoid = "No lo apagues en escucha normal: el corrector quedaría ciego."
    )
    val tonalCurve = SpecInfo(
        title = "ARMONÍA · Curva",
        what = "Curva tonal objetivo: Plana / Harman 2018 / Élite.",
        purpose = "Define hacia qué balance tiende el EQ.",
        whenUse = "Harman 2018 es la referencia; Plana es para medir, no para escuchar.",
        whenAvoid = "No escuches en Plana por defecto: es la referencia de medición, no de disfrute."
    )
    val tonalEq = SpecInfo(
        title = "ARMONÍA · EQ por banda",
        what = "EQ por banda: graves, medios, agudos y presencia (−12..+12 dB).",
        purpose = "Da forma al balance: graves, voz, brillo y aire por separado.",
        whenUse = "Ajusta una banda por vez y compara con bypass al mismo nivel.",
        whenAvoid = "No subas todas las bandas a la vez: eso es subir el volumen con otro nombre."
    )
    val tonalTrim = SpecInfo(
        title = "ARMONÍA · Trim",
        what = "Compensación de nivel (−6..+6 dB).",
        purpose = "Cierra la ganancia neta a 0.0 dB (Matched-LUFS) sin tocar la curva.",
        whenUse = "Solo se toca para cerrar el residual medido; nunca para \"sonar más fuerte\".",
        whenAvoid = "Nunca lo uses para sonar más fuerte: el trim es cierre, no ganancia."
    )
    val tonalLoudness = SpecInfo(
        title = "ARMONÍA · Loudness ISO 226",
        what = "Compensación de sonoridad a volumen bajo.",
        purpose = "A volumen bajo el oído pierde graves y agudos; esto lo compensa.",
        whenUse = "Déjala encendida; apágala solo si altera demasiado el balance.",
        whenAvoid = "No la apagues a volumen bajo: perderás graves y agudos sin darte cuenta."
    )
    val dynPunch = SpecInfo(
        title = "PULSO · Pegada",
        what = "Intensidad de la pegada (0–1).",
        purpose = "Añade golpe a los transitorios.",
        whenUse = "0.0 en Steve Miller (no es rock de pegada); súbela solo en música que lo pida.",
        whenAvoid = "No la subas en música que no la pide: el golpe se vuelve cartón."
    )
    val dynWidth = SpecInfo(
        title = "PULSO · Ancho M/S",
        what = "Ancho estéreo medio/lado (1 = neutro).",
        purpose = "Ensancha la imagen sin mover el centro.",
        whenUse = "1.05 es la referencia; en audífonos evita exagerar si la voz pierde el centro.",
        whenAvoid = "No lo exageres: la voz pierde el centro y el mono se vuelve hueco."
    )
    val dynSafety = SpecInfo(
        title = "PULSO · Limitador",
        what = "Seguridad: limita los picos (0–1, 1 = activo).",
        purpose = "Evita saturación al subir otros controles.",
        whenUse = "Déjalo en 1.0; es tu red de seguridad.",
        whenAvoid = "No lo apagues para 'más dinámica': lo que ganas es saturación, no dinámica."
    )
    val dynComp = SpecInfo(
        title = "PULSO · Umbral / Ratio",
        what = "Umbral (−60..0 dB) y ratio (1–20) del compresor.",
        purpose = "El umbral dice cuándo actúa; el ratio, cuánto reduce.",
        whenUse = "Para música dinámica usa compresión suave (−16 dB / 2.0:1); si suena aplastado, reduce.",
        whenAvoid = "No comprimas fuerte 'por si acaso': una mezcla aplastada no se deshace."
    )
    val dynExciter = SpecInfo(
        title = "PULSO · Exciter",
        what = "Realce de armónicos (0–1).",
        purpose = "Añade presencia y brillo percibidos.",
        whenUse = "0.48 es la referencia; demasiado vuelve ásperas voces y platillos.",
        whenAvoid = "No lo subas para 'más detalle': el exceso se oye como sibilancia."
    )
    val dynVoice = SpecInfo(
        title = "PULSO · Voz",
        what = "Realce de voz/prosodia (0–1).",
        purpose = "Destaca la voz hablada o cantada.",
        whenUse = "0.0 en música; útil en podcast o diálogo, no aquí.",
        whenAvoid = "No lo subas en música: la voz se despega de la mezcla."
    )
    val brainMode = SpecInfo(
        title = "NEURO · Modo AUTO/MANUAL",
        what = "AUTO adapta, MANUAL solo observa.",
        purpose = "AUTO deja que el cerebro gobierne; MANUAL congela todo para comparar.",
        whenUse = "AUTO para escuchar; MANUAL para pruebas A/B repetibles.",
        whenAvoid = "No escuches en MANUAL esperando que el cerebro actúe: en MANUAL solo observa."
    )
    val brainAdaptation = SpecInfo(
        title = "NEURO · Adaptación",
        what = "Fuerza con que el cerebro adapta (0–1).",
        purpose = "Limita cuánto puede mover el cerebro los parámetros.",
        whenUse = "0.5 es la referencia; si percibes bombeo o escena inestable, baja.",
        whenAvoid = "No la subas a 1.0 para 'más cerebro': la escena se vuelve inestable."
    )
    val antiDolby = SpecInfo(
        title = "NEURO · Guardián AntiDolby",
        what = "Telemetría de solo lectura (widener, boost EQ, spread, intensidad).",
        purpose = "Muestra lo que el guardián está haciendo, sin que lo toques.",
        whenUse = "Solo mira: un guion significa \"sin dato válido\", no cero.",
        whenAvoid = "No intentes 'ajustar' al guardián: no tiene controles, es solo lectura."
    )
    val thd = SpecInfo(
        title = "THD+N cadena",
        what = "Distorsión + ruido medidos de la cadena (solo lectura, en FUSIÓN).",
        purpose = "Dice si la cadena está limpia o si algo distorsiona.",
        whenUse = "\"—\" significa sin lectura; no es cero.",
        whenAvoid = "No compares este número con el de otros aparatos: es la cadena activa, no un banco de pruebas."
    )
    val neutralReset = SpecInfo(
        title = "TODO NEUTRO / RESTABLECER",
        what = "TODO NEUTRO apaga la cadena (bit-transparente); RESTABLECER vuelve a los valores de fábrica fusionados.",
        purpose = "Comparar procesado vs. limpio, y volver a un punto conocido.",
        whenUse = "TODO NEUTRO para A/B; RESTABLECER cuando te pierdas entre sliders.",
        whenAvoid = "No pulses RESTABLECER esperando recuperar tu perfil: restaura los valores fusionados, no tu perfil."
    )
    val captureMix = SpecInfo(
        title = "Mezcla captura",
        what = "Cuánto del procesado se reinyecta frente al original de la app que suena al 100%.",
        purpose = "Funde la copia procesada con el original para que no suene como eco.",
        whenUse = "0.60 es el valor vivo; por encima de 0.50 el eco / comb filtering vuelve.",
        whenAvoid = "No la subas por encima de 0.50: el original suena al 100% y la copia ya no se funde como cuerpo."
    )
}
