# Propuesta 0012 — dead-functions-scanner (Plan Omnipotencia)

**Hallazgo:** el scan reportaba 11 funciones muertas en `app/src/main/cpp`.
**Veredicto:** 10 eran falsos positivos del scanner; 1 era real
(`SAF_IsModelLoaded`).

## Diagnóstico (verificado, no inferido)

### Los 10 falsos positivos: bug del scanner
`scan_dead_functions()` buscaba llamadores con `git grep -l` y **excluía
el archivo definidor completo**:

```python
callers = [l for l in refs.splitlines()
           if l and Path(l).resolve() != path.resolve()]
```

Un helper de test que solo se llama dentro de su propio archivo
(DriveEta, Pearson, RmsDiffDb, RmsDiffDbShift, RunOrbe, RunOrbeStereo,
RunHolos, FillHolosState, RunOnce, CheckIdentity) salía como "muerto"
aunque sí se usa. Verificado por grep: cada uno tiene llamadores en su
propio archivo (p. ej. `Pearson` en test_5d_phase0.cpp:235,
`RmsDiffDbShift` en test_neu_compiler.cpp:446-447/556-557,
`RunOnce`/`CheckIdentity` en test_spectral_core.cpp:80-191).

### La 1 real: SAF_IsModelLoaded
`saf_runtime.cpp:45`, retorna `g_saf_model_loaded`. Sin llamadores en
todo el repo (C++, JNI ni Kotlin). `SAF_LoadModel` tampoco se llama
desde C++ (solo lo menciona un comentario en SafModelLoader.cpp:59),
pero el scanner no lo marca porque el comentario cuenta como
"llamador" — debilidad aparte, fuera de este fix.

**Por qué no se cablea:** la pregunta "¿está el modelo SAF
disponible?" ya se responde en Kotlin vía `safAssetAvailable`
(FotoHoloTelemetryBridge, chequeo de asset) y el motor degrada sin
modelo. Cablear `SAF_IsModelLoaded` a JNI + telemetría duplicaría una
vía existente. Decisión: **legacy + exclusión documentada** (regla de
Esse: nunca borrar código).

## El fix (ELEVA, no solo arregla)

1. **Scanner correcto:** un llamador es cualquier mención del nombre
   que NO sea la propia definición (línea del nombre de la función),
   en el mismo archivo o en otro. `git grep -n` + comparación por
   (archivo, línea). Tras el fix: **0 funciones muertas** (antes 11).
2. **SAF_IsModelLoaded → LEGACY:** comentario en saf_runtime.cpp con
   la razón + entrada en `DEAD_FUNCTION_ALLOWLIST` (patrón de
   `STANDALONE_HEADERS`, 0011). Si la razón deja de aplicar, se quita
   del allowlist y el scanner vuelve a exigirla.
3. **Dos bugs del scanner encontrados por los tests y corregidos en el
   mismo parche:**
   - `run()` liga `cwd=REPO` en tiempo de definición: el scanner
     pasaba `cwd=REPO` explícito para que un REPO monkeado funcione.
   - Los paths de `git grep` son relativos a la raíz del repo: se
     resuelven contra `REPO`, no contra el cwd del proceso.

## Validación

- 4 tests nuevos (`tools/evolution/tests/test_scan_dead_functions.py`,
  repo git temporal): llamador en mismo archivo, llamador en otro
  archivo, nunca llamada (muerta), allowlist. **4/4 OK.**
- Scan real tras el fix: **0 funciones muertas**.
- Parche validado en scratch: 0007 + 0012 aplican limpio sobre HEAD;
  tests 0007 (7) + 0012 (4) = **11/11 OK**; ambos scans en 0.

## Dependencia

**0012 requiere 0007 aplicado primero** (ambos tocan
`tools/evolution/evolution_engine.py`). El dry-run de
`apply_proposal.py` será verde una vez 0007 esté commiteado; la
validación manual sobre esa base ya está hecha (ver arriba).

## Archivos

- `tools/evolution/evolution_engine.py` — fix del scanner + allowlist.
- `app/src/main/cpp/saf_runtime.cpp` — comentario LEGACY.
- `tools/evolution/tests/test_scan_dead_functions.py` — 4 tests nuevos.

**Autoría:** coordinador del Plan Omnipotencia, vía directa
(2026-11-16). Sin consejo (el consejo no produce parches precisos;
0/7 aprobaciones).
