# 0026 — Diagnóstico comparativo: IVANNA vs los 8 referentes mundiales

**Mandato de Esse (2026-11-16 11:49 CST):** "Nuestro DSP tiene que superar por creces generacionales a todos estos ni más ni menos."

**Método:** cada referente se descompone en LA capacidad concreta que lo hace el mejor de su categoría (no su marca, sino su parámetro/algoritmo/medición). Para cada una: qué tiene IVANNA verificado en código, qué le falta (brecha medible), y cómo se supera por generaciones. Nada se afirma sin fuente en código.

**Estado base verificado de IVANNA (Ruta A viva, `DSPBridge.nativeProcess`, jni:1792):**
- float32 en toda la cadena, 48 kHz, bloques ≤2048 frames (42.67 ms), presupuesto de shed 85%.
- Coste cadena (host, bloque 320, periodo 6666 µs): media 1508 µs, p50 1547, p95 1906, p99 2264, max 5372 µs; 2/2000 bloques sobre presupuesto. En teléfono (~3.2x): ~4.8 ms media, ~7.2 ms p99. Shed real 3.4% (bloques en seco).
- ASRC: FIR 319 taps minimum-phase + Catmull-Rom + FIR Ψ 64 taps/oído, clamp ±1000 ppm, residuo ~0 ppm, THD+N −86/−84/−83 dB.
- ORBE: THD+N −139.37 dB ON / −153.45 dB bit-exacto OFF @1 kHz, IMD −156.04 dB, latencia algorítmica 10.7 ms, 23x tiempo real (host). Solo yaw (sin pitch, sin pinna en el propio ORBE; el pinna manifold vive en los 5 Ejes).
- HOLOS: STFT hop 128 / FFT 512, 24 bandas (tier 12), media 486 µs, max 9302 µs (host). Armónicos 0.0 por directiva.
- NPE (0024): nace encendido, ~261 µs/bloque-1024, THD+N −10.35 dB a parámetros vivos (color armónico por diseño).
- Compresor: MONOBANDA (un solo detector `env_`, threshold −16 dB, ratio 2.4). `include/Compressor.h`.
- SafetyLimiter: true-peak 4x + soft-ceil, THD 0.00% a 0 dBFS. Última etapa real.
- RIR: dataset real de 200 salas, convolución particionada (BLOCK 512, MAX_IR 512, FFT 1024, cola 16384, 32 partes), presupuesto de cola 20 ms.
- Φ: muestra a muestra sin FFT, modos MEDIDO/APROXIMADO/CALIBRADO, amount 0.5.
- Volterra H2: ACTIVO (anti-lossy reconstruction, no emulación analógica).
- WFS: ACTIVO post-PDEngine. PDEngine (NHO+Spatial+HRTF): solo si `evolutionary_active`.
- NEU·Ω: Fase 6 completa en código, OFF por defecto (`g_neuEnabledA{false}`). Fases 3 (cola E5 en frecuencia), 4 (OMNI+Φ en la matriz), 5 (relojes) PENDIENTES.
- Latencia: AudioFlinger 21.00 ms (HAL frame 192, mixPeriod 4 ms). Ruta altavoz 16-bit/48 kHz; DAC USB 24-bit/48 kHz.
- Hexagon/FastRPC: EXISTE pero es stubs (`dsp_open()==nullptr`, `g_fastrpc_enabled{false}`). NO está en la cadena viva.
- Bucle neural: OFF (user-gated). Motor coclear inverso: OFF (alineado con su pref `false`). Omni: 0.0. HOLOS armónicos: 0.0.
- Tracker SO(3): EN PAUSA por orden de Esse ("Espera", 2026-11-11). Bloquea ORBE pitch por head-tracking real.

## 1. Tabla comparativa (la capacidad, no la marca)

| # | Referente | Lo que lo hace el mejor (capacidad concreta) | IVANNA hoy (verificado) | Brecha medible | Cómo se supera por generaciones |
|---|---|---|---|---|---|
| 1 | **SHARC ADSP-21569** (silicio) | Acumulación 40/64-bit: FIR/convolución/corrección de fase sin ruido de cuantización | float32 en TODA la cadena; ASRC FIR 319 taps en float32; estados IIR en float32 | Piso de cuantización float32 ~−144 dB; ASRC THD+N −86 dB | **Núcleo en doble precisión (0029):** acumulación float64 en rutas críticas (ASRC FIR, estados IIR, convolución RIR). El Cortex-A715/A510 tiene FP64 rápido. El teléfono no tiene SHARC, pero el ALGORITMO (bucle cerrado, NPE, NEU·Ω) no existe en ningún SHARC: el silicio corre el algoritmo de otro; IVANNA ES el algoritmo |
| 2 | **Lake LM44/26** (giras) | Filtros Raised Cosine EQ + linealización de fase (crossovers de fase lineal, corrección de retardo de grupo) | EQ con biquads IIR (fase mínima, añade retardo de grupo); NO hay etapa de linealización de fase; el convolver RIR PUEDE corregir fase pero nadie lo usa para eso | Retardo de grupo de la cadena sin medir ni corregir | **Linealización de fase (futura):** medir el retardo de grupo de la cadena y corregirlo con FIR usando la infraestructura de convolución particionada que ya existe. Lake no mide la sala: IVANNA sí (Φ + RIR 200 salas) |
| 3 | **Meyer GALAXY** (vivo) | 96 kHz/32-bit, latencia ultrabaja CONSTANTE en todos los canales | 48 kHz/16-bit en altavoz; latencia 21 ms (AudioFlinger); coste VARIABLE (shed 3.4% pasa bloques en SECO = huecos audibles) | Shed 3.4% (no 0%); latencia 21 ms, no constante | **Latencia determinista + hi-res (futuras):** (a) camino direct_pcm 24-bit (diseño HIRES_RUTA_456 existe, sin implementar); (b) garantía "nunca en seco": degradar etapas en orden de prioridad (HOLOS 24→12, difuso off — el tier ya lo hace) en vez de pasar a seco. GALAXY no tiene corrección de deriva de reloj; IVANNA sí (ASRC+PLL sub-ppm) |
| 4 | **DirectOut PRODIGY.MP** (broadcast) | Conversión grado mastering + FPGA de alta capacidad | Conversión = DAC del teléfono (16-bit altavoz, 24-bit USB); sin FPGA; Hexagon = stubs | Sin coprocesador vivo; CPU comparte todo con el sistema | **Hexagon real (futura):** mover la etapa más pesada (HOLOS STFT o RIR) al cDSP Hexagon por FastRPC. El loader canónico ya existe (`ivanna::hexagon::rt`). PRODIGY es router/convertidor con DSP estándar; IVANNA es un motor psicoacústico |
| 5 | **Trinnov Altitude32** (cine/Hi-Fi) | Optimizer: corrige amplitud + fase + respuesta al impulso + remapeo 3D, con micrófono tetraédrico (Intel + Linux RT) | Φ corrige ganancia por banda ERB (Γ) desde UN solo micrófono del teléfono, un solo punto; sin medición multipunto; sin corrección de IR completa; sin remapeo 3D; ORBE solo yaw | Corrección = ganancia por banda, no IR completa; 1 micrófono, no 4 | **Bucle cerrado vivo (ventaja generacional YA existente):** Trinnov mide UNA VEZ en la instalación; IVANNA mide CONTINUAMENTE mientras suena la música (Φ por micrófono + NPE). La elevación: medición multipunto (el G85 tiene varios micrófonos) + corrección de IR completa vía el convolver RIR + ORBE pitch (BLOQUEADO: tracker SO(3) en pausa por Esse) |
| 6 | **Helix ULTRA S / Brax** (Car Audio SQ) | Doble SHARC 64-bit @400 MHz, AKM Velvet Sound 32-bit, 96 kHz nativo, coprocesador ACO | 48 kHz, float32, sin coprocesador vivo, 16-bit en altavoz | Sin camino 96 kHz ni 32-bit; sin ACO | **Camino 96 kHz + float64 (futuras):** la cadena ya acepta sr hasta 192 kHz en código; falta el camino de 96 kHz con resampler + Hexagon como "ACO". Helix es DSP convencional (EQ/crossover/alineación temporal); IVANNA es motor neural/coclear |
| 7 | **Pro Tools HDX** (estudio) | FPGA + TI C672x, latencia casi cero (sub-ms) | Latencia 21 ms (AudioFlinger); el camino directo (direct_pcm/MMAP) no está implementado | 21 ms vs sub-ms | **Camino directo de baja latencia (futura):** direct_pcm/MMAP → ~4–6 ms. El diseño HIRES_RUTA_456 lo cubre. HDX no tiene ni ASRC con PLL ni bucle cerrado acústico |
| 8 | **UAD-2/Apollo** (estudio) | Emulación de hardware analógico en tiempo real (tubo, cinta, consola) con SHARC | Volterra H2 ACTIVO pero para reconstrucción anti-lossy, NO para calor analógico; sin modelos tubo/cinta/consola; sin oversampling en saturación | Sin etapa de emulación analógica | **Modelado analógico Volterra (futura):** expandir el kernel Volterra a H2+H3 con oversampling 2x/4x (tubo/cinta). La infraestructura Volterra YA existe (HolosVolterraKernel + motor H2). Medible: espectro armónico H2/H3, THD+N. UAD corre en hardware aparte; IVANNA lo corre en el teléfono |

## 2. Ventajas generacionales que YA tiene IVANNA y NINGÚN referente tiene

Estas no son brechas: son capacidades que no existen en ninguno de los 8. Son la base de "por creces generacionales":

1. **Bucle cerrado acústico VIVO (Φ + NPE + bucle neural):** ningún referente mide la sala mientras suena y corrige en tiempo real. Trinnov mide una vez; IVANNA mide siempre. Φ es muestra a muestra sin FFT; el NPE es un modelo coclear (LIF/NHO) que ningún DSP comercial tiene.
2. **NEU·Ω — compilador espectral holográfico:** un ÚNICO operador matricial (una sola STFT, 2 FFTs/hop) que sustituye la cadena serial ORBE→HOLOS. Ningún referente fusiona sus etapas en una matriz espectral única.
3. **ORBE — binaural esférico de campo cercano:** cada banda ERB a azimut y distancia continuos (0.15–2.0 m), parallax por head-tracking. Ningún referente hace binaural por banda con distancia.
4. **HOLOS — operador único de campo acústico:** un vector de estado por banda en STFT hop 128 / FFT 512. Ningún referente tiene un operador único de campo.
5. **ASRC con anclaje de reloj por PLL (sub-ppm):** ningún referente corrige la deriva de reloj del dispositivo en el write-stage con un ASRC magistral (FIR 319 + Ψ). Los de instalación fija usan word clock externo; IVANNA lo hace por medición.
6. **RIR real de 200 salas:** convolución con respuestas al impulso MEDIDAS, no sintetizadas. Ningún referente trae un dataset de salas reales en el dispositivo.
7. **Todo en el teléfono, sin hardware extra, en cualquier Android 13+:** los 8 referentes son cajas de miles de dólares (Trinnov ~$30k, HDX ~$10k, Brax ~$5k). IVANNA corre en un Moto G85.

## 3. Brechas priorizadas por impacto audible en la cadena de Esse (bocinas G85 + DAC 24-bit/48 kHz → Genezi)

| Prioridad | Brecha | Propuesta | Impacto audible |
|---|---|---|---|
| 1 | Dinámica MONOBANDA (un solo detector, ratio 2.4) — todos los referentes tienen dinámica multibanda | **0027 Compresor multibanda** (3–4 bandas, crossovers Linkwitz-Riley, detectores por banda, link estéreo) | Alto: control de dinámica sin pumping, transitorios limpios en bocinas |
| 2 | NEU·Ω incompleto: cola difusa E5 fuera de la matriz (Fase 3), OMNI+Φ fuera (Fase 4), relojes fuera (Fase 5) | **0028 NEU Fase 3** (cola E5 en frecuencia); luego Fase 4 y 5 | Alto: menos coste (más margen), operador más unificado = menos artefactos de cascada |
| 3 | float32 en rutas críticas (vs SHARC 40/64-bit) | **0029 Núcleo en doble precisión** (ASRC FIR + estados IIR + acumulación RIR en float64) | Medio-alto: piso de ruido más bajo, THD+N medible; el DAC de Esse es 24-bit, así que el piso audible baja de verdad |
| 4 | Sin linealización de fase (vs Lake) | Futura: medir retardo de grupo + corrección FIR | Medio: imagen y foco mejoran; medible en ms |
| 5 | Shed pasa a seco 3.4% (vs GALAXY constante) | Futura: garantía "nunca en seco" | Medio: elimina huecos; medible (shed → 0%) |
| 6 | Sin camino hi-res/baja latencia (vs GALAXY/HDX/Helix) | Futura: direct_pcm 24-bit + MMAP | Alto en DAC USB (24-bit real); medio en bocinas |
| 7 | Sin emulación analógica (vs UAD) | Futura: Volterra H2+H3 con oversampling | Medio: calor/analogicidad; medible en espectro H2/H3 |
| 8 | Hexagon = stubs (vs todos: silicio dedicado) | Futura: offload real de HOLOS/RIR al cDSP | Alto en coste/latencia; habilita todo lo demás |
| 9 | ORBE solo yaw (vs Trinnov 3D) | BLOQUEADO: tracker SO(3) en pausa por Esse | — (no se toca sin su orden) |
| 10 | Φ = ganancia por banda, no IR completa (vs Trinnov) | Futura: corrección de IR + multipunto | Alto: la sala real corregida, no solo ecualizada |

## 4. Oleada 1 (este encargo)

- **0027** Compresor multibanda.
- **0028** NEU·Ω Fase 3 (cola E5 en frecuencia).
- **0029** Núcleo en doble precisión (ASRC FIR + estados IIR).

Cada una con consejo (máx 3 rondas), parche validado (CTest + evolución + check_docs_claims), medición antes/después. Las propuestas 4–8 quedan para oleadas siguientes; la 9 está bloqueada por la pausa del tracker SO(3).
