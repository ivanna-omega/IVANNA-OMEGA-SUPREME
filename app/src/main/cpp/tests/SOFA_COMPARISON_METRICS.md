# SOFA_COMPARISON_METRICS.md — Métricas comunes + criterio de física nueva
(Trabajador C, 2026-11-18. No tocar ORBE·Ω: solo arnés y verificación.)

## Referencia medida

- `MIT_KEMAR_normal_pinna.sofa`: 710 posiciones, 512 taps, 44.1 kHz,
  r = 1.4 m, plano horizontal 72 azimuts cada 5°. SOFA 2.1, SimpleFreeFieldHRIR.
- `kemar.ihr1`: conversión offline por `tools/sofa_to_ihr1.py` (710 pos,
  512 taps, 48 kHz). Validada contra el SOFA en
  `~/workspace/analysis-ivanna/sofa_reference_kemar.py`:
  |dITD onset| máx 2.5 µs, |dILD| máx 0.01 dB, |dILD por banda| máx 0.05 dB.
  El test C++ carga el `.ihr1` con `HRTFBinLoader` (sin HDF5 en host).
- `SofaHRTFLoader` NO lee mediciones: valida la firma HDF5 y reconstruye
  UN par HRIR (128 taps) desde la base SOFA-PCA. No tiene azimut. Usarlo
  como "HRTF medida por azimut" es un error (el test lo fija).

## Convención

Oído izquierdo = receiver 0 (y = +0.09 m; ListenerView (1,0,0), Up (0,0,1)).
ITD = t_derecho − t_izquierdo (µs); positivo = fuente a la izquierda
(az SOFA +90 = izquierda). Azimut SOFA 0..355 → ORBE −180..180 (az>180 → az−360).

## Métricas (por azimut, plano el=0; implementación en `sofa_comparison_metrics.hpp`)

| Métrica | Definición | Notas |
|---|---|---|
| ITD onset (PRIMARIO) | Primer cruce del 5% del pico, interpolación lineal sub-muestra, en µs | Mide el frente de onda directo. Resolución ~2 µs |
| ITD xcorr (SECUNDARIO) | Correlación cruzada plana, lag ±1 ms, pico parabólico | Salta de pico por reflejos de pinna (az=115: 814 µs xcorr vs 492 µs onset); solo diagnóstico |
| ILD banda ancha (dB) | 10·log10(E_L/E_R), energía total de la HRIR | |
| ILD por banda (dB) | Diferencia L−R de energía por banda de octava | 7 bandas, centros 250–16000 Hz, FFT 4096 |
| Espectro magnitud (dB/banda/oído) | 10·log10(energía de banda) | Es la H(f) del modelo vs la medida |
| Distorsión espectral SD (dB) | √media((20log10\|H_modelo\| − 20log10\|H_medida\|)²) por banda | SD_raw incluye offset de ganancia; SD_shape lo quita (media por oído/azimut) y mide solo la forma |
| Correlación | Máx correlación cruzada normalizada modelo vs medida (mismo oído), lag ±1 ms | 1.0 = idénticas salvo escala y retardo |

## Valores de referencia medidos (MIT KEMAR, plano horizontal)

- ITD onset máx |.| = 625.9 µs a az=85°; ITD(90°) = +612.2 µs.
- ILD máx |.| = 17.43 dB a az=110°.
- ITD(0°) = 0.0 µs, ILD(0°) = 0.00 dB (simetría frontal).
- Tabla completa por azimut: `~/workspace/analysis-ivanna/sofa_reference_kemar.csv`.

## ORBE·Ω actual — cómo se mide

Cues de SALIDA ante ruido blanco diótico estacionario (1 s asentamiento +
2 s medida): ITD por xcorr yL vs yR, ILD por cociente de PSD (Welch),
magnitud efectiva por banda = PSD_salida/PSD_entrada. Config: HEADPHONES,
amount=1, distancia 1.4 m, hrtfWet=1, upmix off, WFS off.
En host `hasHrtfDataset()==false`: ORBE usa HRTF analítica, no KEMAR medido.
Si eso cambia, la línea base se re-mide.

Dos métodos NO valen para esta cadena (ambos medidos y documentados en el test):
- Impulso: la cadena es STFT por bloques (FFT 512, hop 128); la respuesta
  al impulso está dominada por latencia de bloque (energía cero en las
  primeras 128 muestras, onset ~440–500, pico ~590) y por el crossfade de
  pose (128 bloques, ~341 ms; sin 1 s de silencio previo el impulso mide el
  banco viejo de az 0 en todos los azimuts). ITD espurio de −983 µs a az=0.
- Welch H(f)=Sxy/Sxx: ORBE es ADAPTATIVO (estima azimut/distancia por banda
  desde la propia señal en `estimateBands` y re-renderiza); no tiene función
  de transferencia. Medido: ITD de −1121 µs a az=0, +3312 µs a 60°/120°,
  xcorr ~0.01 con la HRIR medida. Basura, no medida.
- Tampoco vale la xcorr de banda baja (150–500 Hz) sobre la salida
  estocástica: sin pico definido, satura en el borde ±1 ms (1000.0 µs).
  Para ORBE se usa la xcorr de banda ancha; para HRIRs (medido/esfera/ley)
  la de banda baja sí vale (IRs compactas).

## Resultados finales (2026-11-18, CTest en verde)

Referencia medida MIT KEMAR (r=1.4 m): ITD onset máx 628.6 µs (90°: 617.3);
ITD banda baja 150–500 Hz a 90°: 742.9 µs; ILD máx 17.43 dB (90°: 11.78 dB).

| Columna | \|err ITD\| medio | \|err ILD\| medio | SD forma media |
|---|---|---|---|
| (i) Esfera rígida (A, física existente) | 12.8 µs (banda baja) | 1.16 dB | 5.24 dB |
| (ii) Ley de Curvatura (B, teoría nueva) | 78.9 µs (banda baja) | — (solo divergencia) | n/a (sin espectro) |
| (iii) ORBE·Ω actual | 268.7 µs (xcorr) | 6.63 dB | 16.05 dB |

- Esfera: ILD por banda a 90° medido 2.5/4.1/5.7/5.9/8.3/16.4/23.3 dB vs
  esfera 0.1/1.0/2.6/3.5/4.3/5.4/7.3 dB — subestima en agudos (bright spot
  en el oído contralateral). Sin pinna: no hay notches 5–10 kHz.
- Ley de Curvatura: a 90°/r=1.4 m predice 663.8 µs vs 742.9 medido (−79.1);
  a 90°/r=0.5 m (TU-Berlin) 678.2 vs 740.5 (−62.3). Por azimut a 1.4 m:
  errores −90 (30°), −140 (60°), −79 (90°), −165 (120°). La inversión
  distancia-desde-exceso con el exceso MEDIDO estima r=0.128 m (real 1.4 m)
  y r=0.132 m (real 0.5 m): FALLA — el exceso medido (+87/+85 µs sobre
  Woodworth) es difracción de baja frecuencia, no curvatura.
- ORBE actual: ITD de salida 437 µs a az=0 (medido 0) — pedestal de +416 µs
  que viene del par maestro PCA reconstruido (su xcorr ITD es 416.0 µs;
  `synthesizeHrtf` añade 0 retardo a az=0). ILD de salida 1.9–3.4 dB
  (medido hasta 14.4 dB). SD forma ~16 dB. HRTF-solo (amount=0) es casi
  igual: el error vive en la HRTF analítica, no en ORBE.
- Hallazgo: la esfera rígida (física de 1907) supera a ORBE·Ω actual en
  TODAS las métricas contra SOFA medido.

## Veredictos (criterio (a)+(b)+(c)) — REVISADO 2026-11-18 ~19:40 CST

- **A — esfera rígida: NO es física nueva verificable, es re-derivación.**
  (a) ecuaciones escritas sí, pero son la física clásica (Rayleigh 1907),
  no una teoría propia; (b) ninguna predicción distinta de la literatura;
  (c) verificada contra SOFA con excelentes errores. Re-derivación exacta
  y útil — pero re-derivación.
- **A — Ley Geodésica del Notch: SÍ es física nueva verificada (a nivel
  de modelo).** (a) ecuaciones propias escritas sí (ΔL_j(φ) = M_j −
  B_j·sin(φ−φ0_j), f_N = c/(2ΔL), operador unificado H = H_esfera ·
  F_pinna); (b) predicción distinta sí — la migración del notch con la
  elevación (6.4 → 11.2 kHz de −40° a +70°, familia baja), que la esfera
  NO puede predecir en absoluto (su |H| es suave, sin notches); (c)
  verificada por C de forma INDEPENDIENTE: extracción propia del SOFA
  (error máx 583 Hz contra la ley, tolerancia ±650 Hz) y LOO refitado
  por C en Python: BAJA RMS 119 Hz / máx 185 Hz (A afirma 147/278),
  ALTA RMS 464 Hz / máx 990 Hz (A afirma 445/904), contra nulo
  1718/1242 Hz. El operador unificado abre el notch frontal de 8 kHz
  (dip 11.0 dB medido por el arnés, medido real 30.1 dB: posición
  correcta, profundidad subestimada, como admite A). Límites honestos:
  parámetros ajustados a un solo KEMAR (no medidos en la anatomía);
  interpolativa en elevación; azimut de primer orden; profundidad
  aproximada (ρ constantes); la SD de forma por octava NO mejora con
  el pinna (5.73 → 6.74 dB a elev 0: el notch es estrecho y el rizado
  de F_pinna añade error de forma). No es una ley fundamental nueva de
  la naturaleza; es un marco nuevo para la HRTF, verificado donde se
  afirma. Informe de A: `~/workspace/analysis-ivanna/PHYSICAL_HEAD_MODEL_2026-11-18.md`.
- **B — Ley de Curvatura: formulación nueva, verificación PARCIAL.**
  (a) ecuaciones propias escritas sí; (b) predicción distinta sí (ley
  cerrada en κ, rama C exacta ITD=(a/c)·2θ, inversión distancia-desde-exceso);
  (c) la curva ITD(r,θ) se comprueba contra SOFA con error sistemático
  −62…−165 µs (mejora a Woodworth en solo 8–23 µs; la esfera, física
  existente, la supera: +11 µs a 90°), y la predicción usable central
  (distancia desde exceso de ITD) queda REFUTADA por SOFA medido
  (0.13 m estimado vs 1.4/0.5 m reales). B mismo lo dice en su mapa §3.5:
  "NO es física fundamental nueva". Veredicto: **no es física nueva
  verificada**; es una formulación nueva útil cuya predicción central no
  se sostiene contra SOFA.
- **Equipo: SÍ se logró física nueva verificada — la Ley Geodésica del
  Notch (A), a nivel de modelo.** La esfera es re-derivación exacta; la
  Ley de Curvatura es formulación nueva con predicción central refutada.
  Dicho sin inflar: se logró física nueva en el pabellón (notches), no
  en la propagación ni en la cabeza como cuerpo.

## Criterio "física nueva verificada" (directiva de Esse 2026-11-18)

Una teoría es física nueva verificable solo si cumple (a)+(b)+(c):

- (a) Ecuaciones propias ESCRITAS (no solo descripción verbal).
- (b) Al menos UNA predicción cuantitativa DISTINTA de la física existente
  (p. ej. un notch de pinna que la esfera rígida no predice, o parallax de
  campo cercano + ITD de campo lejano con la misma ecuación), con su valor numérico.
- (c) Esa predicción se COMPRUEBA contra SOFA o contra ORBE·Ω con un error
  reportado en estas métricas.

Si no cumple (a)+(b)+(c): veredicto **"no es física nueva verificable,
es re-derivación"**. Si el equipo solo re-deriva la física existente, el
reporte final dice: **"no se logró física nueva, solo se re-derivó la existente"**.
Sin inflar números ni presentar lo existente como nuevo. La verdad es lo primero.

## Tabla final (3 columnas)

(i) física existente re-derivada | (ii) teoría nueva propuesta |
(iii) HRTF medida / ORBE·Ω actual — con los números de error de cada una.
Entregada 2026-11-18 en `test_sofa_comparison.cpp::ComparisonTableForNewPhysics`
y en el informe `~/workspace/analysis-ivanna/TRABAJADOR_C_VEREDICTO_2026-11-18.md`.
