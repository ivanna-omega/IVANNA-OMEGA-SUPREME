# Banco adversarial de escucha y medición — IVANNA

Material de prueba diseñado para **romper** el DSP, no para confirmar que suena
bien. Cada caso caza un modo de fallo concreto (smearing, ringing, modulación
residual, decorrelación, memoria, divergencia entre rutas). El banco mide, no
opina: cada métrica tiene un umbral de fallo explícito y justificado, y el
harness devuelve código de salida 0 (pasa) / 1 (falla) / 2 (error de uso o E/S).

**Regla de uso:** el banco es un detector, no un objetivo. Optimizar el motor
para que pase estos casos concretos sin mejorar el sonido real es hacer trampa
contra uno mismo. Ver la separación entrenamiento / validación / escucha ciega
al final.

Todo corre en host (sin teléfono): C++17, sin dependencias externas, 48 kHz,
PCM 16-bit estéreo (el generador escribe WAV; el harness lee WAV).

## Archivos

| Archivo | Qué hace |
|---|---|
| `src/wav.hpp` | Lectura/escritura WAV PCM 16-bit, 48 kHz, 1–2 canales (header-only). Valida RIFF/PCM/48 kHz/16-bit. |
| `src/adversarial_core.hpp` / `.cpp` | API: generador de los 21 casos, umbrales, medición, eventos. FFT radix-2, Goertzel, coherencia de Welch, análisis de eventos. |
| `src/adversarial_gen.cpp` | CLI generador: `list`, `generate`, `all`, `transform`. |
| `src/adversarial_harness.cpp` | CLI medición: `measure` (JSON a stdout, resumen humano a stderr; salida 0/1/2). |
| `tests/test_adversarial_harness.cpp` | Tests unitarios sin framework externo (159 checks). |
| `scripts/run_harness_case.sh` | Driver de ctest: genera → transforma → mide un caso. |
| `CMakeLists.txt` | Proyecto CTest independiente (C++17, g++-14, -O2 -Wall -Wextra). |

## Matriz de casos (26)

Todos los casos son estéreo, 48 kHz. Las semillas son deterministas
(FNV-1a del ID xor la semilla pedida); `--variant mono` usa la misma semilla
en ambos canales, `--variant stereo`/`seed` usa una distinta en R.

| # | Caso | Dur. | Estímulo | Qué fallo caza |
|---|---|---|---|---|
| 1 | `click_01` | 3 s | Click de 1 muestra a −0.1 dBFS en 1.0 s | Smearing, limiter lento, pérdida de transitorio |
| 2 | `impulse_01` | 3 s | Impulso de 1 muestra a −1 dBFS | Respuesta impulsional, ringing, overshoot |
| 3 | `impulse_02` | 3 s | Impulso de 8 muestras con ventana Hann, energía −3 dBFS | Ataque realista y preservación temporal |
| 4 | `train_01` | 4 s | Tren de 4 ráfagas a 2/4/8/16 kHz, 250 ms, 0/−3/−6/−12 dB | Estado dependiente del historial (el 4º transitorio no debe sonar distinto por los 3 anteriores) |
| 5 | `bass_01` | 15 s | Seno 20 Hz a −6 dBFS, 12 s | Inestabilidad subgrave, DC, saturación del integrador |
| 6 | `bass_02` | 15 s | 30+40+60 Hz a −12 dBFS, fases 0/π/π/2 | Intermodulación, modulación de ganancia |
| 7 | `sibilant_01` | 5 s | Ruido de banda 5.5–10.5 kHz, envolvente 8 ms ataque / 70 ms release, −12 dBFS, 4 ráfagas | Sibilancia, aliasing, aspereza |
| 8 | `dense_01` | 10 s | 32 senos logarítmicos 80 Hz–18 kHz, fases aleatorias, RMS −12 dBFS en la región del tono | Error acumulado, THD+N, modulación residual |
| 9 | `dense_02` | 30 s | Ruido rosa + tonos 60/250/1k/4k/12k Hz, RMS −14 dBFS | Comportamiento con material denso realista |
| 10 | `phase_01` | 5 s | L = seno 1 kHz, R = seno 1 kHz invertido | Coherencia intercanal; la suma mono debe cancelar |
| 11 | `phase_02` | 5 s | L/R iguales con R retardado 0.25 ms (1–3 s) y 0.5 ms (3–5 s) | Desfase temporal y errores de fase por canal |
| 12 | `binaural_01` | 5 s | Pulso 700 Hz con ITD ±0.30 ms e ILD ±6 dB (espejo a los 3 s) | Preservación espacial, estados independientes por canal |
| 13 | `silence_01` | 5 s | Tono 1 kHz −12 dBFS 1 s, silencio 500 ms, tono de nuevo | Colas, memoria residual, reactivación |
| 14 | `noise_imp_01` | 10 s | 200 impulsos aleatorios de 1–3 muestras, −30 a −1 dBFS | Robustez frente a ruido impulsivo |
| 15 | `level_01` | 6 s | Escalones −60/−40/−24/−12/−3/−0.1 dBFS, 500 ms c/u, seno 1 kHz | Attack/release, cuantización, clipping |
| 16 | `level_02` | 5 s | Rampas −60 a −0.1 dBFS en 50 ms y vuelta | Overshoot, bombeo, estabilidad del control |
| 17 | `ood_01` | 8 s | DC 0.2 + seno 5 Hz + seno 23.5 kHz, pico total −6 dBFS | Supuestos fuera de distribución, filtros mal condicionados |
| 18 | `ood_02` | 5 s | Seno 997 Hz a amplitud 1.05 recortado a ±1 | Protección ante entrada ya distorsionada |
| 19 | `route_01` | 5 s | Tono 1 kHz −12 dBFS + click a −0.1 dBFS en 1.0 s (marcador) | Recuperación JNI tras desconexión de 300 ms |
| 20 | `route_02` | 5 s | Mismo estímulo | Estado obsoleto y reconvergencia tras congelar el snapshot 500 ms |
| 21 | `route_03` | 10 s | Mismo estímulo (10 s) | Divergencia entre Ruta A y B con snapshots desplazados 1–4 hops |
| 22 | `voice_01` | 6 s | Voz seca sintética (f0 120 Hz, armónicos con formantes 700/1200 Hz, 4 sílabas de 300 ms + sibilante) | Voz sin reverberación; aliasing, aspereza, colas añadidas |
| 23 | `drums_01` | 6 s | Bombo (seno 60 Hz, caída exponencial) y caja (ruido 1–4 kHz, caída) a 120 BPM | Preservación de ataques rítmicos y transitorios de batería |
| 24 | `hotmix_01` | 10 s | Ruido rosa + 8 tonos, RMS −6 dBFS, pico limitado a −0.5 dBFS (crest ~6 dB) | Cadena bajo entrada caliente y limitada (master moderna) |
| 25 | `pause_01` | 6 s | Tono 1 kHz 2 s, silencio 1 s (pausa), tono 2 s | Click de reanudación y estado tras una pausa (TIDAL) |
| 26 | `srchange_01` | 5 s | Tono 1 kHz continuo 5 s; el runner re-prepara la cadena a otra tasa a 2.5 s | Discontinuidad por cambio de sample rate |

Los casos `route_*` solo aportan el estímulo con marcador; la
desconexión / congelamiento / desplazamiento la ejecuta el runner del DUT
(ver "Contrato del RouteRunner").

## Métricas

El harness compara la referencia (entrada) con la salida del DUT y emite una
métrica por línea JSON: `{name, value, applicable, pass, detail}`.
`applicable=false` significa que esa métrica no se pudo medir en este par
(por ejemplo, THD+N sin ventana estacionaria): **no es un fallo**, es una
medición ausente. Todas las métricas se miden sobre el canal L salvo que se
indique lo contrario.

| Métrica | Definición |
|---|---|
| `finite` | La salida no contiene NaN ni Inf (1.0 = limpia). |
| `peak_ref_dbfs` / `peak_dut_dbfs` | Pico absoluto de referencia y salida, en dBFS. |
| `overshoot_db` | `peak_dut − peak_ref` en dB. |
| `rms_ratio_db` | `20·log10(RMS_dut / RMS_ref)` sobre la señal activa. |
| `latency_samples` | Retardo del DUT en muestras. Método: onset por nivel (−60 dBFS) + refinado por correlación de **|x|** (ver abajo). |
| `latency_corr` | Correlación normalizada en el retardo medido (1.0 = copia exacta). |
| `latency_peak_samples` | Retardo medido por el pico del transitorio (solo si el caso tiene un pico claro, pico/RMS ≥ 10). |
| `latency_jitter_spread_samples` | Dispersión intra-archivo del retardo por ventanas (proxy; el p99 de 100 repeticiones lo mide el runner). |
| `thdn_db` | THD+N a la frecuencia `--freq` sobre una ventana estacionaria de ciclos enteros: `10·log10((P_total − P_fund) / P_fund)`, con P_fund por Goertzel. |
| `phase_error_deg` | Error de fase del DUT respecto a la referencia a `--freq`, en la misma ventana estacionaria (grados). |
| `interchannel_phase_ref_deg` / `interchannel_phase_dut_deg` | Fase R−L a `--freq` en referencia y salida (informativo; verifica `phase_02` y `binaural_01`). |
| `residual_modulation_db` | Variación de la envolvente: amplitud de Goertzel por sub-ventanas de 10 ciclos dentro de la ventana estacionaria; `20·log10((máx − mín) / media)`. |
| `click_energy_loss_db` | Pérdida de energía del transitorio en ±1 hop alrededor del pico (negativo = pérdida; el reporte emite `−loss` para que el umbral sea directo). |
| `transient_centroid_shift_hops` / `_samples` | Desplazamiento del centroide temporal del transitorio (hops de 128 muestras y muestras). |
| `rise_time_ref_samples` / `rise_time_dut_samples` | Tiempo de subida del transitorio (informativo). |
| `overshoot_attack_db` | Overshoot alrededor del ataque (±1 hop). |
| `coherence_dut` / `coherence_ref` | Coherencia magnitud-cuadrado de Welch entre L y R, promediada 100 Hz–12 kHz (segmentos 4096, hop 2048, ventana Hann). |
| `coherence_check` | La salida no debe decorrelar una entrada coherente: es `coherence_dut` cuando la referencia es coherente; `n/a` si la referencia ya es incoherente o si `--expect-low-coherence`. |
| `mono_residual_dbfs` | RMS de (L+R)/2 en `phase_01`: la suma mono debe cancelar (informativo fuera de ese caso). |
| `residual_silence_dbfs` | RMS de la salida en silencio, 100 ms tras la última señal. |

### Por qué la latencia se mide con |x| y no con la forma de onda

La correlación directa de la forma de onda confunde una inversión de fase con
un retardo de medio periodo: un seno invertido mide "24 muestras de latencia"
(la mitad de 48) cuando el retardo real es 0. El harness correlaciona **|x|**
(invariante ante inversión de fase) y, entre los retardos casi-óptimos
(correlación dentro de 1e-4 del máximo), elige el más cercano al onset por
nivel. El onset se busca a nivel de muestra (primer índice con |x| > −60 dBFS
3 muestras seguidas), con el onset RMS de 1 ms como respaldo.

### Por qué la ventana estacionaria es a nivel de muestra

La fórmula THD+N compara la potencia total con la de la fundamental. Si la
ventana contiene aunque sea un 1% de silencio, la fundamental se diluye y el
THD+N espurio es de **−20 dB** en un passthrough perfecto. Por eso la ventana
se coloca dentro de la región activa continua con límites medidos muestra a
muestra (fin del tono = primer índice tras el cual hay 48 muestras seguidas
por debajo de −60 dBFS) y se ajusta a un número entero de ciclos. Verificado:
un passthrough WAV 16-bit mide THD+N = **−87.07 dB**, que es el piso real de
cuantización 16-bit a −12 dBFS (≈ −86 dB), no un artefacto de ventana.

### Por qué la modulación residual es por envolvente y no `y − LPF(y)`

El diseño inicial proponía medir la modulación residual como `y − LPF(y)` con
un promedio móvil. Eso no funciona: un promedio móvil no elimina un tono (un
seno de 1 kHz pasa casi intacto por un LPF de 10 Hz solo si la ventana es
mucho más larga que el periodo; en la práctica el residuo mide el tono
mismo, no su modulación). La definición correcta es la de la envolvente: la
amplitud del tono (Goertzel) por sub-ventanas de 10 ciclos debe ser constante
si la entrada es constante; su variación es la modulación residual.

## Umbrales de fallo (justificación)

Son umbrales **operativos iniciales**, no metas de calidad: un DUT correcto
pasa con margen y uno roto falla con claridad. Cada uno se justifica; ninguno
es un número redondo inventado.

| Umbral | Valor | Justificación |
|---|---|---|
| Desvío de latencia vs baseline | ≤ 1 muestra | La latencia algorítmica fija del motor (FFT, lookahead) NO se penaliza: se compara contra una **latencia dorada** (baseline) de la misma build y ruta, medida con el mismo caso. Un desvío > 1 muestra es un cambio real de comportamiento (re-arme, re-seed, ruta distinta), no jitter de medición. Sin baseline, solo se aplica el límite absoluto. |
| Latencia absoluta | ≤ 8192 muestras (170 ms) | Por encima de 170 ms el DUT está roto con o sin baseline (la latencia de un efecto en vivo no puede ser esa). |
| Jitter p99 | ≤ 32 muestras JNI (0.667 ms), ≤ 64 daemon (1.333 ms) | Un hop son 128 muestras (2.667 ms). El jitter debe ser una fracción pequeña del hop para que la salida no se desincronice de la ventana de análisis; el daemon tiene el doble de margen porque cruza un proceso. El harness mide un proxy intra-archivo; el p99 de 100 repeticiones lo mide el runner (`--jitter-limit` lo sobrescribe). |
| Error de fase | ≤ 2° (100 Hz–18 kHz), ≤ 5° (20–100 Hz) | 2° a 1 kHz son 5.6 µs de desfase intercanal, por debajo del umbral de localización audible (~10 µs ITD); en graves la longitud de onda es mayor y 5° sigue siendo inaudible como desplazamiento. |
| Overshoot | ≤ 0.5 dB | Por encima de 0.5 dB el DUT añade ganancia donde no la había; el límite de pico absoluto (−0.1 dBFS) es la protección contra clipping. |
| Pico absoluto | ≤ −0.1 dBFS | Margen de 0.1 dB bajo el full-scale digital; un pico por encima es clipping o ganancia no pedida. |
| THD+N | ≤ −80 dB (seno a −12 dBFS) | El piso de cuantización 16-bit a −12 dBFS es ≈ −86 dB (medido: −87.07 dB en passthrough WAV). Un DUT que procese en 16-bit no puede bajar de ahí; −80 dB deja 6 dB de margen sobre el piso y exige que el procesado no añada distorsión audible por encima del ruido de cuantización. |
| Silencio residual | ≤ −90 dBFS | En silencio, 100 ms tras la última señal, la salida debe estar por debajo de −90 dBFS: cualquier cola, oscilación o ruido de fondo del motor por encima es audible en una pausa. |
| Modulación residual | ≤ −70 dB | Con entrada constante, la envolvente de salida no debe variar más de −70 dB: una modulación de amplitud por encima es un temblor audible (AM) que el motor introduce. |
| Pérdida de energía del transitorio | ≤ 1 dB | Un transitorio que pierde más de 1 dB de energía en ±1 hop ha sido suavizado (smearing) por el procesado. |
| Desplazamiento del centroide | ≤ 0.25 hop (32 muestras) | El centroide temporal del transitorio no debe moverse más de un cuarto de hop: más es un retardo o un pre-eco introducido. |
| Coherencia intercanal | ≥ 0.995 (100 Hz–12 kHz) | Una entrada coherente debe salir coherente: por debajo de 0.995 el DUT está decorrelando (phasiness, imagen inestable). No aplica si la referencia ya es incoherente o si se pasa `--expect-low-coherence`. |
| Residuo mono (`phase_01`) | ≤ −55 dBFS | L y R invertidos deben cancelar en mono hasta −55 dBFS (límite práctico por cuantización 16-bit y redondeo); por encima, el DUT no trata los canales simétricamente. |
| Reconvergencia | ≤ 3 hops JNI, ≤ 8 hops daemon | Tras una desconexión, el estado aplicado debe volver a igualar al pedido en ≤ 3 hops por JNI (8 ms) o ≤ 8 hops por daemon (21 ms); más es un hueco audible. |
| Divergencia de estado | ≤ 2 hops | El estado aplicado no puede diferir del pedido más de 2 hops (5.3 ms); más es un motor que no obedece. |
| Persistencia de estado | ≤ 100 ms | Un estado pedido no puede persistir más de 100 ms tras ser cambiado. |
| Snapshot obsoleto | ≤ 2 hops | Un cambio pedido cuya snapshot no se actualiza en más de 2 hops es un snapshot congelado. |

Los umbrales se sobrescriben por línea de comandos en el harness
(`--baseline N`, `--jitter-limit N`, `--thdn-limit DB`) sin tocar el código.

## Contrato del RouteRunner (casos route_01/02/03)

El generador solo produce el estímulo con marcador (tono 1 kHz −12 dBFS +
click a −0.1 dBFS en 1.0 s). El runner del DUT debe ejecutar la perturbación
y registrar los eventos. Los tres casos:

- **`route_01` — desconexión JNI:** el runner desconecta la Ruta A 300 ms
  (a partir de ~1.0 s, sobre el marcador) y la reconecta. Se mide la
  reconvergencia: hops desde la reconexión hasta que `applied == requested`.
- **`route_02` — snapshot congelado:** el runner congela el snapshot del
  daemon 500 ms y luego salta de estado. Se mide el snapshot obsoleto (cambio
  pedido sin actualización de snapshot > 2 hops) y la reconvergencia.
- **`route_03` — divergencia entre rutas:** el runner alimenta Ruta A y B con
  snapshots desplazados 1–4 hops durante 10 s. Se mide la divergencia máxima
  entre lo pedido y lo aplicado en cada ruta y el tiempo que tarda en
  converger.

### Esquema `events.jsonl`

Una línea JSON por evento (el parser es mínimo y solo entiende este esquema):

```json
{"t_capture_ns": 123456789, "route": "jni", "requested": {"mode": 3, "strength": 0.8}, "applied": {"mode": 2, "strength": 0.75}, "connected": true, "snapshot_id": 1842}
```

| Campo | Tipo | Significado |
|---|---|---|
| `t_capture_ns` | entero | Timestamp de captura, en nanosegundos (monótono). |
| `route` | string | `"jni"` o `"daemon"`. |
| `requested.mode` / `requested.strength` | entero / número | Estado pedido por la app (modo y fuerza). |
| `applied.mode` / `applied.strength` | entero / número | Estado realmente aplicado por el motor. |
| `connected` | booleano | `false` durante una desconexión. |
| `snapshot_id` | entero | ID del snapshot del daemon (debe avanzar con cada cambio; si se congela mientras `requested` cambia, es un snapshot obsoleto). |

El harness (`--events FILE --route jni|daemon`) emite el reporte de
divergencia: `max_strength_diff`, `mode_mismatch_count`, `max_gap_ms`,
`max_gap_hops`, `stale_snapshot`, `reconvergence_hops`, `pass`.

## Cómo pasar un DUT por el banco

```bash
# 1. Compilar (g++-14, C++17)
cmake -S tests/adversarial -B /tmp/build-adversarial -DCMAKE_CXX_COMPILER=g++-14
cmake --build /tmp/build-adversarial -j

# 2. Generar el material (todo el banco, o un caso)
./adversarial_gen all /tmp/bank
./adversarial_gen generate click_01 /tmp/bank/click_01.wav --seed 0

# 3. Pasar cada WAV por el DUT y guardar la salida con el mismo nombre
#    (el DUT es un programa aparte: el motor, un plugin, un script; el banco
#    no lo ejecuta). La salida debe tener la misma longitud que la entrada.

# 4. Medir cada par (referencia, salida)
./adversarial_harness measure /tmp/bank/click_01.wav /tmp/out/click_01.wav \
    --case click_01 --route jni
#    JSON a stdout, resumen humano a stderr, salida 0 = pasa, 1 = falla.

# 5. Con baseline de latencia dorada (misma build y ruta, medida antes)
./adversarial_harness measure ref.wav dut.wav --case silence_01 \
    --freq 1000 --baseline 128

# 6. Con eventos de ruta (route_01/02/03)
./adversarial_harness measure ref.wav dut.wav --case route_01 \
    --route daemon --events /tmp/out/events.jsonl
```

Para verificar el harness mismo sin un DUT real, el generador tiene un modo
`transform` que aplica un retardo, una ganancia o una inversión a un WAV:

```bash
./adversarial_gen transform ref.wav dut.wav --delay 128 --gain-db -3 --invert
```

Los tests de ctest usan ese modo como DUT sustituto (passthrough, retardo de
128 muestras, inversión, ganancia −3 dB) y comprueban que el harness mide lo
que debe y que el passthrough bit-exacto pasa.

## Separación estricta: entrenamiento / validación / escucha ciega

El banco solo sirve si no se optimiza para él. Tres conjuntos disjuntos,
definidos por la **semilla** del generador (`--seed N`):

| Conjunto | Semillas | Variación | Uso |
|---|---|---|---|
| Entrenamiento | 0–99 | Ninguna (parámetros nominales) | Desarrollar y depurar el motor. Se puede mirar, medir y ajustar contra él. |
| Validación | 100–199 | Frecuencias ±3%, duraciones ±20% (el runner las aplica al generar) | Verificar que un fix generaliza y no es un ajuste al caso nominal. No se ajustan umbrales contra él. |
| Escucha ciega | 200–999 | Semilla aleatoria por caso, sin publicar los WAV | Evaluación final y escucha ABX. Los WAV ciegos **no se publican ni se guardan en el repo**; solo se guarda un manifiesto SHA-256 de los archivos generados, para poder verificar después qué se midió sin revelar el material. |

Reglas:

1. **Nunca se ajustan los umbrales de fallo contra el conjunto ciego.** Si un
   DUT falla en ciego, se arregla el motor, no el umbral.
2. **Nunca se entrena ni se calibra contra el conjunto ciego.** La escucha
   ciega se hace una vez por versión, con el material generado en ese momento.
3. **El manifiesto SHA-256** (`sha256sum` de cada WAV ciego + la semilla y el
   caso) se guarda junto al reporte de medición; así se puede reproducir la
   evaluación sin guardar el audio.
4. Los casos con parámetros fijos (frecuencias, niveles, ITD/ILD) son los
   nominales; la variación de validación es pequeña a propósito (±3% de
   frecuencia, ±20% de duración) para que el caso siga cazando el mismo fallo
   y no se convierta en otro caso distinto.

## Qué requiere grabación real (no sintetizable)

El generador sintetiza lo que se puede; cuatro cosas necesitan audio real y
aquí van solo como aproximación sintética, documentada como tal:

| Necesidad real | Aproximación sintética en el banco | Qué falta |
|---|---|---|
| Voz sibilante real (`sibilant_01`) | Ruido de banda 5.5–10.5 kHz con envolvente de sibilante (8 ms ataque / 70 ms release), 4 ráfagas | La estructura formántica y la coarticulación de una /s/ real; la aproximación caza aliasing y aspereza, no la naturalidad de la voz. |
| Mezclas densas reales (`dense_02`) | Ruido rosa + 5 tonos, RMS −14 dBFS | La dinámica y la densidad espectral de una mezcla musical real (compresión, transitorios, estéreo). **El nivel está aproximado por RMS −14 dBFS; la calibración LUFS exacta es trabajo posterior.** |
| Música con silencios abruptos (`silence_01`) | Tono con silencio de 500 ms entre repeticiones | El contexto musical (la cola de una reverberación real cortada de golpe, el ruido de fondo de una grabación). |
| Binaural real (`binaural_01`) | Pulso 700 Hz con ITD 0.30 ms e ILD 6 dB | Una HRTF real y una fuente en movimiento; la aproximación verifica que el motor preserva ITD/ILD, no la calidad espacial. |

Cuando se disponga de esas grabaciones, se añaden como casos nuevos
(`*_real`) con el mismo harness, sin tocar los sintéticos.

## Correcciones al diseño inicial (luna, gpt-5.6-luna)

El diseño de partida vino de una consulta a cxa/gpt-5.6-luna (Bifrost); estas
correcciones se hicieron al verificarlo con tests, y son parte del diseño:

1. **`dense_01`: el crest factor de ~3 dB es inalcanzable.** 32 senos
   logarítmicos con fases aleatorias se comportan como una señal gaussiana:
   el crest factor real es **~11 dB** (medido: 11.03 dB en la región del tono,
   12.00 dB en todo el archivo), no 3 dB. Ninguna elección de fases lo baja a
   3 dB (se probaron 10 semillas y se quedó la mejor). El generador
   normaliza el RMS sobre la región del tono (−12 dBFS) y el test exige
   crest factor en [8, 13] dB sobre esa región. El objetivo de 3 dB era un
   error del diseño inicial.
2. **Modulación residual: por envolvente, no `y − LPF(y)`.** Un promedio móvil
   no elimina un tono; la medida correcta es la variación de la amplitud de
   Goertzel por sub-ventanas de 10 ciclos (ver "Métricas").
3. **Latencia: correlación de |x|, no de la forma de onda.** La correlación
   directa confunde una inversión con un retardo de medio periodo (ver
   "Métricas").
4. **Ventana estacionaria a nivel de muestra.** Un 1% de silencio dentro de
   la ventana falsea el THD+N en −20 dB (ver "Métricas").
5. **`add_burst` usa tiempo relativo.** Con tiempo absoluto, desplazar el
   inicio de una ráfaga solo movía la envolvente, no la portadora, y el ITD
   medido era 0° en vez de −73.5°. Ahora una ráfaga desplazada es una copia
   retardada de verdad.
6. **El lector WAV y el `measure` completo son propios.** El diseño inicial
   traía el lector WAV omitido y `measure` como stub; ambos se implementaron
   y se verifican con los 159 checks unitarios.

## Build y tests

```bash
cmake -S tests/adversarial -B /tmp/build-adversarial -DCMAKE_CXX_COMPILER=g++-14
cmake --build /tmp/build-adversarial -j
ctest --test-dir /tmp/build-adversarial --output-on-failure
```

Resultado de referencia (2026-11-22, P1): **7/7 tests ctest, 159/159 checks
unitarios**. Los 7 tests: unitario, generación de los 26 casos, y 5 casos de
harness (passthrough de click, passthrough de seno, retardo de 128 muestras,
inversión —que debe fallar la fase—, ganancia −3 dB —que debe fallar el
overshoot/RMS—). Los dos últimos son **WILL_FAIL a propósito**: comprueban
que el harness detecta el defecto, no que el DUT pase.

## P1 — Matriz bypass vs IVANNA vs cada operador (2026-11-22)

El banco standalone mide un DUT contra una referencia. **P1** lo conecta a
la FusedChain REAL: `app/src/main/cpp/tests/test_p1_adversarial_matrix.cpp`
(12 tests CTest en la suite principal) pasa 11 casos por 9 configuraciones
(bypass, IVANNA completa, cada operador solo con el safety limiter siempre
activo) y escribe `p1_matrix_<caso>.json` por caso. `scripts/p1_aggregate.py`
consolida los 11 JSON en tablas (defectos, pico, RMS) y defectos por operador;
el reporte de referencia vive en `docs/p1_reporte_2026-11-22.md`.

El test solo falla por defectos (no-finito, clipping digital > 0 dBFS, DC
añadido ≥ 1e-2, latencia > 8192, silencio residual > −90 dBFS); THD+N, fase,
RMS, coherencia y pérdida de transitorio se REPORTAN, no fallan (la cadena
cambia el sonido por diseño; sin P0 no hay línea base para un umbral absoluto
de smearing).

**Hallazgos P1 (medidos 2026-11-22):** APEX suma el transitorio DESPUÉS del
safety limiter, así que en hotmix_01 la salida IVANNA llega a +0.81 dBFS
(APEX OFF: −0.14 dBFS, contenido por el limiter). NEURO deja cola residual
de −77 dBFS tras el fin de la señal. 89/99 configuraciones sin defecto; 3
tests en rojo (Bass, Phase, HotMix) por esos 2 defectos reales del motor.
Su fix es un cambio de motor aparte. El bucle de 100 corridas
(`scripts/p1_loop100.sh`, estado en `p1_loop100_state.txt`) prueba
determinismo hasta que el motor se arregle.

**P0 (REFERENCE_LOCKED) sigue sin existir:** P1 detecta regresiones, no
sustituye la referencia congelada.

## CI

El banco corre en su propio job `adversarial-bank` de
`.github/workflows/tests-host.yml` (7 tests, ~1 min). Es un carril propio:
no se mezcla con la suite principal de CTest (que tiene su propio conteo y
sus puertas) y no la bloquea, pero un fallo aquí no se ignora: o el harness
detectó un defecto real, o el harness está roto. Se dispara con cada push
que toque `tests/**` o `app/src/main/cpp/**` (las rutas del workflow).

## Estado

- Generador y harness completos y verificados en host.
- Pendiente: calibración LUFS exacta de `dense_02` (hoy RMS −14 dBFS).
- Pendiente: casos `*_real` con grabaciones (voz sibilante, mezcla densa,
  música con silencios, binaural real).
- Pendiente: el RouteRunner del DUT (la perturbación de ruta y el
  `events.jsonl` los produce el runner; el banco solo define el contrato y
  analiza el registro).
- Pendiente: latencia dorada (baseline) por build y ruta — la mide el runner
  en la primera pasada y se guarda junto al reporte.
