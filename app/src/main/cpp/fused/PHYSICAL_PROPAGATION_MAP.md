# ORBE·Ω — De heurística a física: mapa de reemplazo y Ley de Curvatura

Trabajador B (propagación de onda / render físico), 2026-11-18.
Módulo: `fused/physical_propagation.hpp/.cpp` · Test: `tests/test_physical_propagation.cpp`.
Solo local. `fused_spatial.cpp` NO se tocó (el camino caliente sigue intacto;
el cableado es trabajo futuro, con fallback — ver §6).

---

## 1. Mapa de reemplazo (el piso: heurística → ley física)

| # | Heurística en `fused_spatial.cpp` (línea) | Ley física que la deriva | Reemplazo en el módulo | Veredicto |
|---|---|---|---|---|
| (a) | `HrtfManager::setSceneDistance` — ley 1/d con rampa, identidad en 1.0 m; en realidad `HrirPoseSelect::sceneDistanceGain`: 1/d clampado a [0.5, 1.41254] (+3 dB / −6 dB) (`fused_spatial.cpp:76-80`, `HrirPoseSelect.hpp:50`) | Divergencia esférica, solución de la ecuación de onda: presión ∝ 1/r | `sphericalSpreadingGain(r)` (pura) y `sceneDistanceGainPhysical(r)` (drop-in con los mismos topes) | **Derivable.** La ley 1/r es física exacta. Los topes NO son física: son seguridad (evitan recorte cerca y −∞ lejos). Además la ganancia actual es SOLO nivel: no modela campo cercano espectral/ILD — eso lo cubre la Ley de Curvatura (§3), no el 1/r. |
| (b) | Mezcla WFS de potencia constante: `gWet=sin(t·π/2)`, `gDry=cos(t·π/2)` (`fused_spatial.cpp:3`, rampa por bloque) | Conservación de potencia: dry²+wet²=1 | `constantPowerMix(t)` | **Parcialmente derivable.** La conservación de potencia es física para señales NO correlacionadas. Seco y WFS provienen de la MISMA señal (parcialmente correladas), así que el sin/cos es convención de mezcla psicoacústica, no ley de la onda. Se conserva como está, documentado. |
| (c) | Modulación de apertura WFS por análisis de escena: `widthFactor = 0.3 + 0.9·sideRatio`, `apertura = 1.5·wfsSpread·widthFactor` (`fused_spatial.cpp:3`); misma fórmula duplicada en `StereoObjectDecomposer::canonicalLayout` | Ninguna: `sideRatio` es energía mid/side MEDIDA del contenido | — (sin reemplazo físico) | **No derivable: decisión de contenido.** La física fija dónde van los objetos una vez elegida la apertura (geometría + delay `d/c` + driving 1/√d, ya en `WfsRenderer::computeTapTargets`), pero QUÉ tan ancha es la escena la decide la música, no la onda. Los coeficientes 0.3/0.9 son arbitrarios (arte/tuning). Quedan como decisión de contenido, explícita. |
| (d1) | `OrbeParams.proximityDb = 3.0f` (`fused_spatial.cpp:89`) | Divergencia esférica por oído (1/d por camino) + sombra cefálica; el estante de proximidad es `nearfield::proximityShelfDb`, acotado a +6 dB | `nearFieldParallax()` (caminos por oído) + `curvatureLawIldDivergenceDb()` | **Mixto.** El estante en sí deriva de la física (la divergencia por oído da hasta 14 dB de ILD a 0.15 m/90° — medido en §5). El valor 3.0 dB es un TOPE de tuning elegido a mano: la física no fija ese número. Reemplazo: el tope se puede derivar de la distancia (p. ej. estante( r, f ) real), pero el 3.0 fijo queda como decisión de contenido hasta que se cablee. |
| (d2) | `OrbeParams.diffuse = 0.5f` (`fused_spatial.cpp:90`; en `OrbeBinaural::renderBins`: `outL = mid·gL·phL + side·diffuse`) | Ninguna | — (sin reemplazo físico) | **No derivable: decisión de contenido/mezcla.** Cuánto "side" pasa al render directo es una elección estética de espacialidad, no una consecuencia de la ecuación de onda. Queda como parámetro de contenido. |
| (e) | Distancia de objetos WFS fija `kWfsObjectY = 1.5 m` (`fused_spatial.hpp:95`) | Geometría WFS: 1.5 m = radio del array virtual `kArrayRadiusM` | `propagationDelaySeconds()` / `sphericalSpreadingGain()` para sus delays/ganancias | **Derivable como geometría, no como número.** Poner los objetos SOBRE el círculo del array es la frontera natural WFS (fuente enfocada en el array); el 1.5 m en sí es un parámetro de diseño del array, no una constante física. Coherente, no arbitrario: si cambia el radio del array, debe cambiar con él. |
| (extra) | Driving WFS 1/√d en `WfsRenderer::computeTapTargets` | Onda cilíndrica 2.5D (Spors & Ahrens) | `cylindricalSpreadingGain(r)` | **Ya era física.** Se documenta la distinción: fuente puntual 1/r (HRTF) vs WFS 2.5D 1/√d — ambas correctas en su régimen; confundirlas sería el error. |

---

## 2. Verificación numérica del piso (valores reales, no fabricados)

| Ley | Predicción | Medido (test) | Fuente independiente |
|---|---|---|---|
| 1/r | −6.02 dB al duplicar distancia | −6.0206 dB | identidad matemática |
| retardo | 1 m → 2.915 ms; 48 kHz → 139.94 muestras | exacto | r/c, c = 343 m/s |
| c(T) | c(20 °C) = 343.21 m/s | 343.21 | 331.3·√(1+T/273.15) |
| ISO 9613-1, 20 °C/70 % HR | α = 4.98 / 23.09 / 77.63 dB/km @ 1/4/8 kHz | 4.98 / 23.09 / 77.63 | tablas ISO publicadas (tolerancia ±10 %, la de la norma) |
| ISO 9613-1, 10 °C/70 % HR | α = 3.7 / 32.8 / 117 dB/km | 3.61 / 33.00 / 118.32 | tablas ISO publicadas |
| ISO, 200 m | 1.00 dB @ 1 kHz; 15.53 dB @ 8 kHz | 1.00 / 15.53 | tabla acmap (motor ISO 9613-1) |
| Doppler | 1 kHz, v_r = +10 m/s → 1030.03 Hz | 1030.03 | f·c/(c−v_r) |
| Parallax vs `NearFieldHeadModel` | mismo algoritmo, doble vs float | < 2 µs en todo azimut, r ∈ {0.15, 0.35, 1, 2} m | modelo existente del repo |

Hallazgo honesto del piso: a distancias ORBE (0.15–2 m) la absorción del aire
es despreciable (8 kHz a 2 m: 0.16 dB). No se cablea como efecto audible en
campo cercano; su sitio es la sala / campo lejano (SALA VIVA), no ORBE.

---

## 3. LEY DE CURVATURA DE ORBE (física nueva: formulación)

### 3.1 El problema que ataca

Hoy el audio espacial trata campo lejano (onda plana: HRTF, Woodworth,
ITD/ILD) y campo cercano (onda esférica: parallax, cues de distancia) como
dos regímenes con heurísticas distintas — en ORBE, el modelo numérico de
rayos (`NearFieldHeadModel`) por un lado y Woodworth por otro. La Ley de
Curvatura los unifica en UNA sola ecuación parametrizada por la curvatura
local del frente de onda en la cabeza, κ = 1/r, sin cambio de régimen.

### 3.2 Ecuaciones (explicitadas)

Parámetro adimensional: ε = a·κ = a/r (a = 0.0875 m, radio KEMAR).
Azimut plegado θf ∈ [0, π/2] por antisimetría izquierda/derecha y espejo
frontal/trasero; signo = signo(θ).

Ángulo de cambio de rama: θ* = asin(ε) — el azimut donde el rayo directo
al oído ipsilateral deja de estar bloqueado por la esfera.

**Rama C (|θf| ≤ θ*, ambos oídos por onda reptante) — EXACTA, no serie:**

> ITD = (a/c) · 2·θf

(Demostración: las longitudes de tangente son idénticas para ambos oídos y
se cancelan; los arcos difieren en 2θf. En θf = 0 da 0 exacto, como exige
la simetría.)

**Rama D (θ* < |θf| ≤ π/2, ipsilateral directo + contralateral reptante):**

> ITD = (a/c)·[ θf + sinθf + (ε/2)·sin²θf − (ε²/2)·sinθf·cos²θf
> + ε³·(1/6 + (5/8)sin⁴θf − (3/4)sin²θf)
> + ε⁴·((7/8)sin⁵θf − (5/4)sin³θf + (3/8)sinθf)
> + ε⁵·((21/16)sin⁶θf − (35/16)sin⁴θf + (15/16)sin²θf − 1/20) + O(ε⁶) ]

**Límite κ → 0 (ε → 0):** ITD → (a/c)(θf + sinθf) = Woodworth exacto.
**Continuidad:** ambas ramas coinciden en θ* hasta O(ε⁵) (verificado
simbólicamente con sympy y numéricamente: salto < 2 µs).

**ILD por divergencia (misma geometría, forma cerrada):**
ILD_div = 20·log10(P_L/P_R), con P los caminos de rama.

**Inversión (el cue nuevo):** a 90°, ITD = (a/c)(1 + π/2 + ε/2 + O(ε³)).
El exceso sobre Woodworth mide la distancia SIN usar nivel:
ε ≈ 2·E·c/a, r = a/ε, con E = ITD − ITD_Woodworth.

**Corolario de acercamiento (looming):** κ̇ = κ²·v_r; τ = 1/(κ·v_r).

### 3.3 Predicciones verificables (con números)

| Predicción | Valor ley | Valor referencia independiente | Estado |
|---|---|---|---|
| ITD 90°, r = 100 m (κ→0) | 655.93 µs | Woodworth 655.82 µs (0.017 %) | ✓ |
| ITD 90°, r = 0.15 m | 732.5 µs (+11.7 % sobre Woodworth) | Modelo exacto de rayos 732.6 µs; header NFHM "~+12 %" | ✓ |
| ITD 90°, r = 0.35 / 1 / 2 m | 687.9 / 667.0 / 661.4 µs | Modelo exacto, mismo valor ±0.1 µs | ✓ |
| Ley vs rayos, todo azimut (−180°..180°, paso 5°) | error máx. 0.59 µs @ 0.15 m; 0.00 µs @ ≥0.35 m | `NearFieldHeadModel::itdSeconds` | ✓ |
| Rama C exacta: 30°, 0.15 m | 267.1 µs | Modelo exacto 267.1 µs | ✓ |
| ILD divergencia 90° | 14.01 dB @ 0.15 m; 5.57 @ 0.35; 1.94 @ 1 m; 0.02 @ 100 m | Modelo exacto de caminos | ✓ |
| Distancia desde exceso ITD (90°) | r = 0.15→0.145; 0.35→0.348; 0.5→0.499; 1.0→0.999; 2.0→2.000 m | r real (exceso medido con el modelo exacto, no con la ley) | ✓ |
| Looming | r = 0.5 m, v_r = 1 m/s → κ̇ = 4.0 /m/s, τ = 0.50 s | τ = r/v_r (identidad) | ✓ |

### 3.4 Qué explica que la física actual aplicada no explica

ORBE hoy estima la distancia por banda desde el ILD (`distanceFromIld`):
eso necesita calibración de nivel y un modelo de sombra cefálica, y en
graves el ILD de campo lejano es ~0, así que no hay cue. La ley predice un
cue de distancia **independiente del nivel y válido en graves**: el exceso
de ITD sobre Woodworth, con inversión en forma cerrada. Es el cue que la
cadena actual no usa. Uso propuesto: reemplazar/complementar
`distanceFromIld` por `distanceFromItdExcess` en la estimación por banda
de ORBE (trabajo futuro, con escucha; no cableado en este entregable).

### 3.5 Veredicto de honestidad (directiva de Esse: la verdad primero)

- **NO es física fundamental nueva.** La ecuación de onda no cambia. La
  geometría subyacente es el modelo de cabeza esférica por rayos (Kuhn
  1977; Duda & Martens 1998), que ORBE ya implementa numéricamente en
  `NearFieldHeadModel`. Quien diga que "nadie unificó campo cercano y
  lejano antes" miente: el modelo esférico ya es continuo en r.
- **SÍ es una formulación nueva y verificable para el render:** (1) la
  ley cerrada en κ con dos ramas que se unen continuamente en θ* = asin ε
  y recuperan Woodworth exactamente en κ → 0 — sin trazado numérico de
  rayos, sin cambio de régimen, evaluable en O(1) por banda; (2) el
  resultado exacto de la rama C, ITD = (a/c)·2θ; (3) la inversión
  distancia-desde-exceso-ITD como cue usable, independiente del nivel.
  Ninguna de las tres estaba escrita así en el repo ni en la forma
  estándar de aplicar el modelo esférico (que es numérico, no cerrado).
- **Límites conocidos:** la ley da la parte de DIVERGENCIA del ILD; la
  sombra por difracción (alta frecuencia) NO está en la serie de rayos y
  sigue necesitando el modelo numérico o HRTF medida (a 0.15 m/90° la
  divergencia da 14.01 dB; el ILD total medido en graves llega a ~15–20 dB
  según el header NFHM — el resto es difracción). La serie en ε converge
  más lento cuanto mayor es ε; a r = 0.15 m (ε = 0.58) el error residual
  es 0.59 µs con O(ε⁵) — aceptable, pero el extremo near-field (r < 0.15 m)
  queda fuera del dominio verificado.
- **No verificado contra SOFA medido:** la verificación es contra el
  modelo exacto de rayos del repo y contra Woodworth, no contra HRTF
  medidas SOFA (no hay dataset SOFA en el repo local accesible a este
  trabajador). La predicción a 90°/0.15 m (+11.7 %) es comprobable contra
  cualquier dataset near-field SOFA/KEMAR: si el ITD medido no supera a
  Woodworth en ~+12 %, la ley cae. Esa es la prueba que la cierra.

## 4. Segunda dirección explorada: sistema acoplado oyente–campo

Formulada y DESCARTADA como física nueva (resultado negativo honesto).
Cantidad conservada propuesta: el flujo total de potencia acústica a
través de la esfera de la cabeza es invariante bajo rotación de la cabeza
(la esfera es simétrica; la fuente estática no cambia). La ley de render
que se derivaría — "al rotar la cabeza, los cues deben transformar como
ITD(θ−Δψ), campo anclado al mundo, no a la cabeza" — es EXACTAMENTE lo
que ORBE ya implementa vía `setHeadPose()` + head-tracking, y la
normalización de potencia constante por banda en `renderBins`
(`norm = sqrt(2/(gL²+gR²))`) ya impone la conservación por banda. No hay
ley nueva extraíble: el sistema acoplado colapsa al anclaje al mundo que
ya existe. Se documenta para no re-explorarlo.

## 5. Qué NO se hizo (alcance)

- `fused_spatial.cpp` NO se tocó: el camino caliente sigue intacto. El
  cableado (usar `sceneDistanceGainPhysical` en `setSceneDistance`,
  `distanceFromItdExcess` en la estimación ORBE por banda) es trabajo
  futuro con fallback al camino actual y validación por escucha.
- No se pusheó nada a GitHub. No se borró código existente.
- No se presentan la ley 1/r ni la absorción ISO como física nueva: son
  el piso (§1–§2), no la meta (§3).

## 6. Cableado futuro sugerido (con fallback)

1. `HrtfManager::setSceneDistance` ← `sceneDistanceGainPhysical(r)`:
   mismo valor que hoy (drop-in); el cambio real es documentar los topes
   como seguridad, no física.
2. `OrbeBinaural::estimateBands` ← `distanceFromItdExcess(ITD_medido −
   Woodworth)`: estimador de distancia por banda independiente del nivel;
   mantener `distanceFromIld` como fallback si el exceso ≤ 0 o θf lejos
   de 90° (la función devuelve −1.0 en ese caso).
3. `WfsRenderer::computeTapTargets` ← `propagationDelaySeconds` /
   `cylindricalSpreadingGain`: mismo valor que hoy (el 1/√d ya estaba);
   el cambio es nominal (una sola fuente de verdad para las leyes).
4. Validación antes de cablear: SOFA near-field (prueba que cierra §3.5)
   + escucha de Esse en Genezi.
