# Protocolos F4 y F6 — Medición real (PENDIENTES, no ejecutados en VM)

Fase 2, ítem E (v2.5.472). Estos protocolos NO se ejecutaron: necesitan el
Moto G85, el Sony Genezi y (para F6) hardware EEG que no está en la VM.
No hay ninguna medición inventada en este documento: solo qué medir, con
qué, y el criterio de éxito que decide si algo se publica o queda en
fail-closed.

## F4 — Medición real de los POR MEDIR

### F4a. Kernel GENEZY01 del Sony Genezi MHC-PZ1D (barrido ESS de Farina)
- Qué medir: respuesta lineal `h1(t)` + impulsos no lineales `h2(t)`,
  `h3(t)` del Genezi (VACS/GROOVE + sala) con el micrófono del Moto G85.
- Con qué: barrido exponencial 20 Hz → 20 kHz, T=10 s, generado en el
  G85 (`ess_lo_48k.wav` / `ess_hi_48k.wav`), por AUDIO IN del Genezi
  (47 kΩ, sin pasar de 700 mV RMS). Grabar con el micrófono del G85 a
  15 cm del woofer/puerto (captura no lineal) y luego en el punto de
  escucha (captura `H_b` + T60 para Ω). Script: `~/workspace/genezi_physics_id.py`.
- Separación temporal (un solo barrido, filtro inverso analítico):
  `h1` en t=0; `h2` aislado en Δt2 = −T·ln2/ln(f2/f1) = −1.0034 s;
  `h3` aislado en Δt3 = −1.5902 s.
- Prueba de pureza del micrófono (Criterio 2:1 dB): correr el barrido a
  dos niveles separados 6.0 dB (−18 dBFS y −12 dBFS). En `h1` la
  diferencia debe ser +6.0 dB; en `h2`, exactamente +12.0 dB (2× en dB).
  Si `h2` sube +12 dB, es física real del Genezi (Bl(x)/Kx(x)), no ruido
  ni artefacto del micrófono.
- Criterio de éxito: el kernel `.ivk1` predice THD/IMD de un multi-tono
  no usado en el ajuste (80+130+310 Hz) con error < 1 dB por armónico,
  con pre-distorsión K2 OFF vs ON (el pico de H2 en 160–620 Hz debe caer
  mediblemente). Si no: `K_b` permanece como identidad (fail-closed) y
  no se publica como kernel físico.

### F4b. Δ_b de metadatos insignia
- Qué medir: delta de curva objetivo de los productos insignia
  (firmas electroacústicas medidas publicadas) respecto a Harman 2018.
- Con qué: metadatos / curvas publicadas de los productos de referencia
  (sin hardware nuevo).
- Criterio: Δ_b documentado por banda ERB con fuente citada; sin fuente,
  Δ_b = 0 (no se inventa).

### F4c. Curva α(L) de ILD a alto SPL
- Qué medir: pre-expansión ILD 1/α(L) a 85–110 dB SPL.
- Con qué: app/script de calibración en el propio G85 + Genezi:
  reproducir tonos/ruido a 3 volúmenes del Genezi (75, 90, 102 dB) y
  ajustar con un deslizador el balance ILD hasta centrar la imagen
  lateral (Experimento F1 de FORMULACION_MAGISTRAL_PSI.md).
- Criterio: curva α(L) medida en 3 puntos con repetibilidad (dos
  sesiones); sin medida, α(L) = 1 (sin pre-expansión).

### F4d. Foto + tarjeta (antropometría del oyente, $0)
- Qué medir: radio de cabeza `a` (reemplaza 0.0875 m), elipticidad
  (corrige el +12% de sesgo ITD de la esfera) y coordenadas
  (L_base,j, d_j) de la concha para GeodesicPinna3D.
- Con qué: 1 foto frontal y 1 lateral con el G85, con una tarjeta
  estándar (85.6 mm) junto a la oreja como escala métrica.
- Criterio: parámetros extraídos con escala verificable; sin foto, se
  usan los valores KEMAR por defecto (no se inventan medidas).

## F6 — Lazo 5D (BioObserver / EEG)

- Qué medir: viabilidad del lazo cortical pasivo con hardware MW75 Neuro
  (EEG de consumo, ~$500 USD — el único elemento del plan que requiere
  dinero; NO se ha comprado).
- Con qué: MW75 Neuro + BioObserver (5D): TRF 1–8 Hz (η), ASSR 40 Hz,
  fatiga α/β. Prueba de viabilidad: ASSR 40 Hz + ρ_env > 0.1.
- Criterio de éxito: Δρ_env ≥ +0.10 y resonancia C(f) ≥ 1.5× en 1–8 Hz
  correlacionada con SAQI (r > 0.5). Si pasa, cerrar lazo lento
  (0.5 dB/s) sobre Γ_b.
- Si falla: si ρ_env ≤ 0.1 en vivo, archivar el 5D como inobservable en
  EEG de consumo y dejar solo el observador de fatiga α/β. Sin hardware
  EEG no hay ninguna afirmación sobre el lazo 5D.

## F7 (referencia, fuera de este ítem)

Puerta Auditiva Final (ABX): escucha ciega en Moto G85 (niveles
igualados ±0.2 LU, 6 fragmentos, ≥16 ensayos, replicado en 2 días).
GREEN: ≥12/16 aciertos (p < 0.01), preferencia ≥ +15, y ninguna
dimensión del índice VISCERAL cae >10 puntos. Sin GREEN replicado, todo
queda OFF por defecto (amount = 0 bit-exacto).
