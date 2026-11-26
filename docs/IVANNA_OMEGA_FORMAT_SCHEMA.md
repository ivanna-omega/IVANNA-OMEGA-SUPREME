# Formato IVANNA OMEGA — Schema v1.0 (perfil por canción)

Estado: **perfil por canción** (1 archivo real: `voodoo_child.ivanna.json`).
La palabra pública es **"perfil por canción"**, no "formato", hasta tener
3 archivos válidos (ver §5). Documento del refinamiento P6+P7, 2026-11-26.

## 1. Qué es

Un archivo `.ivanna.json` es el **perfil medido de UNA canción**: su huella
acústica (`fingerprint`, medida del FLAC real) + los parámetros del motor
IVANNA derivados de esa medida (`engine`) + la procedencia de cada
derivación (`derivation`, texto auditable). No es un preset genérico: cada
archivo nace de un audio real y no se inventa.

Referencia normativa: `voodoo_child.ivanna.json` (Voodoo Child, FLAC
48 kHz/24-bit, 313.07 s). Validador: `scripts/validate_ivanna_profile.py`
(solo stdlib; `python3 scripts/validate_ivanna_profile.py archivo.ivanna.json`).

## 2. Reglas generales

- JSON, UTF-8, un solo objeto raíz. Sin comentarios ni claves duplicadas.
- `format` debe ser exactamente `"IVANNA OMEGA"` y `format_version`
  exactamente `"1.0"`.
- **Regla de Oro Matched-LUFS: `engine.net_gain_db` debe ser `0.0`**
  (tolerancia 1e-9). Un perfil con ganancia neta ≠ 0.0 dB es inválido.
- Claves desconocidas (en cualquier nivel) son **error** en v1.0: el schema
  es estricto; una v1.1 podrá añadir campos opcionales.
- Los números son JSON numbers (no strings). Los booleanos son JSON booleans.
- `derivation.*` son strings de procedencia obligatorios: un perfil sin
  procedencia no es auditable y es inválido.

## 3. Campos

### 3.1 Raíz (todos obligatorios)

| Campo | Tipo | Valor / rango |
|---|---|---|
| `format` | string | `"IVANNA OMEGA"` (exacto) |
| `format_version` | string | `"1.0"` (exacto) |
| `track` | string no vacío | título de la canción |
| `artist` | string no vacío | artista |
| `source` | string no vacío | archivo de origen + formato, p. ej. `"voodoo_child_ivanna_omega_6_1fes.flac (48 kHz / 24-bit, estereo)"` |
| `duration_s` | number | > 0 (segundos del audio medido) |
| `fingerprint` | objeto | §3.2 |
| `engine` | objeto | §3.3 |
| `derivation` | objeto | §3.4 |

### 3.2 `fingerprint` — huella medida del audio real (todos obligatorios)

| Campo | Tipo | Rango |
|---|---|---|
| `loudness_lufs_integrated` | number | [-70, 0] LUFS (BS.1770 + gating, ver derivation) |
| `loudness_rms_dbfs` | number | [-90, 0] dBFS |
| `peak_dbfs` | number | (-120, 0] dBFS (pico real ≤ 0 dBFS) |
| `crest_db` | number | [0, 40] dB (peak − RMS) |
| `spectrum_db_rel` | objeto, 9 claves §3.2.1 | cada valor [-80, 0] dB relativo |
| `transient_energy_ratio` | number | [0, 1] |
| `tonal_energy_ratio` | number | [0, 1]; `transient + tonal == 1.0` (±0.01) |
| `stereo_correlation` | number | [-1, 1] |
| `stereo_width_side_mid` | number | [0, 2] |
| `sample_rate_hz` | integer | uno de 44100, 48000, 96000, 192000 |
| `channels` | integer | 1 o 2 |

#### 3.2.1 `spectrum_db_rel` — 9 bandas fijas (todas obligatorias)

`sub`, `low_60_120`, `lowmid`, `mid_500_1k`, `highmid`, `high_2k_4k`,
`presence_4k_8k`, `air`, `ultra`. Cada valor en dB relativo a la banda
de referencia del análisis (valores observados: −5 a −37 dB).

### 3.3 `engine` — parámetros IVANNA derivados (todos obligatorios)

| Campo | Tipo | Rango |
|---|---|---|
| `orbe_amount` | number | [0, 1] |
| `orbe_distance_m` | number | [0.15, 2.0] m (rango ORBE) |
| `holos_amount` | number | [0, 1] (el motor recorta a [0, 1]; 1.0 es el máximo) |
| `phi_amount` | number | [0, 1] |
| `volterra_enabled` | boolean | — |
| `volterra_mix` | number | [0, 1] |
| `eq_bands_db` | objeto, 4 claves §3.3.1 | cada valor [-2, 2] dB (clamp de derivación) |
| `compressor` | objeto §3.3.2 | — |
| `exciter_amount` | number | [0, 1] |
| `trim_db` | number | [-12, 12] dB |
| `net_gain_db` | number | **exactamente 0.0** (Regla de Oro) |
| `structural_split` | objeto §3.3.3 | — |

#### 3.3.1 `eq_bands_db` — 4 bandas fijas (todas obligatorias)

`low_60_120`, `mid_500_1k`, `high_2k_4k`, `presence_4k_8k`.
Cada valor [-2, 2] dB: la derivación documentada corrige suave hacia
plano con factor −0.35 × desviación de la media y clamp ±2 dB. Un valor
fuera de ±2 dB no viene de ese procedimiento y es inválido.

#### 3.3.2 `compressor`

| Campo | Tipo | Rango |
|---|---|---|
| `threshold_db` | number | [-60, 0] dB |
| `ratio` | number | [1, 20] |
| `attack_ms` | number | (0, 1000] ms |
| `release_ms` | number | (0, 5000] ms |

#### 3.3.3 `structural_split`

| Campo | Tipo | Rango |
|---|---|---|
| `transient_amount` | number | [0, 1] (cantidad de procesado, NO ratio de energía) |
| `tonal_amount` | number | [0, 1] (cantidad de procesado, NO ratio de energía) |
| `transient_energy_ratio` | number | [0, 1]; debe coincidir con `fingerprint.transient_energy_ratio` (±0.001) |
| `tonal_energy_ratio` | number | [0, 1]; debe coincidir con `fingerprint.tonal_energy_ratio` (±0.001) |

Nota: `transient_amount` (0.46) + `tonal_amount` (1.0) = 1.46 en el
archivo de referencia: **no** suman 1.0. Son cantidades de procesado
HPSS, no partición de energía. La partición de energía es
`transient_energy_ratio` + `tonal_energy_ratio` = 1.0.

### 3.4 `derivation` — procedencia (todos obligatorios, strings no vacíos)

| Campo | Contenido |
|---|---|
| `eq` | procedimiento de la corrección EQ (fórmula + clamp) |
| `compressor` | procedimiento del umbral/ratio |
| `structural_split` | procedimiento HPSS (STFT, ratios) |
| `loudness` | procedimiento de loudness (BS.1770, gating) |
| `note` | qué constantes son defaults del motor y qué se derivó de la medición |

## 4. Compatibilidad

- v1.0 es la única versión. Un lector debe rechazar `format_version`
  distinto de `"1.0"` (no adivinar compatibilidad hacia adelante).
- Los campos `engine.*` que son constantes por defecto del motor
  (orbe/holos/phi/volterra/exciter en el archivo de referencia) se
  documentan como tales en `derivation.note`; solo EQ, compresor y split
  se derivan de la medición en v1.0.

## 5. Estado: perfil por canción, no formato (gate)

Hoy existe **1 archivo válido** (`voodoo_child.ivanna.json`, validado con
el script de §1). Faltan 2 para poder hablar de "formato" en público.

**Gate — qué falta para los 2 perfiles restantes:**

1. **Firehouse (KISS):** falta el FLAC real. Esse debe pasarlo (de Qobuz).
   Sin el audio no se puede calcular el perfil: NO se inventa.
2. **Tercer perfil:** falta elegir la canción Y su audio real (FLAC).
   Candidatos sin audio: ninguno válido hasta tener el archivo.

**Audio NO utilizable que existe en el workspace (verificado 2026-11-26):**

- `analysis-ivanna/omega_safmus/*.wav`: tonos sintéticos de control
  (SAFMUS), no música: sin track/artista, no dan un perfil por canción.
- `your_files/barrido_extraido_del_video.wav`: barrido extraído de un
  vídeo, no una canción.

Cuando lleguen los FLAC, el procedimiento es el mismo que produjo el
archivo de referencia (ver `derivation.*` del archivo): medir huella
BS.1770 + espectro 9 bandas + HPSS, derivar EQ/compresor/split, validar
con el script y contar 3 archivos antes de decir "formato" en público.
