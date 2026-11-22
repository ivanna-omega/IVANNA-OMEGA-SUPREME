# Propuesta 0014 — globals-symmetry-check (Plan Omnipotencia)

**Hallazgo:** los 11 archivos `*_controls_bridge.cpp` / `*_globals_effect.cpp`
(los atomics `g_*` entre el proceso app y audioserver) estaban en la lista
de 19 módulos sin tests, y NO existía ningún guard que verificara la
simetría que la lección 389 exige.

## Diagnóstico (verificado, no inferido)

Lección 389 (2026-11-10): cada atomic nuevo `g_*` necesita definición en
AMBOS lados; si falta el stub, el NDK falla con `undefined symbol` y la
CI no publica (la 389: `g_orbe_head_radius_m` solo estaba en el bridge).

Estado medido 2026-11-16 (nombres+tipos+valores iniciales):
- **Simétricos (idénticos):** holos 19, hyper 3, orbe 7, wfs 11.
- **hrtf:** app en `audio_orchestrator.cpp` (no en bridge),
  effect en `hrtf_globals_effect.cpp`, mismos valores
  (`g_hrtf_wet_dry{1.0f}`, `g_hrtf_flush_req{false}`). Su comentario
  confirma un fix previo por `undefined symbols` — misma clase de bug.
- **App-only (intencional, verificado por git grep):**
  omni `g_omni_audiogram_dirty`, `g_omni_yaw_rad` (solo JNI app);
  upmixing `g_upmixing_enabled`, `g_upmixing_immersivity` (solo
  IvannaFusionCore/JNI app). Ninguno se referencia en el effect.

## El fix: guard de regresión

`scripts/check_globals_symmetry.py` (nuevo):
1. Pares bridge/effect (holos, hyper, orbe, wfs): definiciones
   idénticas (nombre, tipo, valor inicial).
2. hrtf: app (`audio_orchestrator.cpp`) vs effect, mismo criterio.
3. App-only (`APP_ONLY`): no deben aparecer referenciados en el
   effect (`omega_effect.cpp` + `*_globals_effect.cpp`).
4. El registro `APP_ONLY` debe ser exacto (cada entrada existe en su
   bridge; si la razón deja de aplicar, el check lo dice).
5. Un atomic nuevo en un solo lado sin registro → hallazgo.

Falla con exit 1 si hay asimetría; [PASS] si no. Sin falsos positivos:
los 4 casos asimétricos reales están clasificados en el registro.

## Validación

- Check en el repo real: **[PASS] 0 asimetrías (4 pares + hrtf + 4 app-only)**.
- 7 tests nuevos (`test_check_globals_symmetry.py`): extracción
  (ignora comentarios), baseline, valor que difiere, lado que falta,
  app-only referenciado en effect, asimetría no documentada, y el
  check real. **7/7 OK.**
- El check es standalone (`python3 scripts/check_globals_symmetry.py`).

## Follow-up (no parte de este parche)

Cablear el check como 7º chequeo host en `scan_host_checks_red()`
(`tools/evolution/evolution_engine.py`) para que corra en el scan de
la auditoría. No se hizo aquí para no encadenar otra dependencia
sobre 0007/0012 (mismo archivo).

## Archivos

- `scripts/check_globals_symmetry.py` — check nuevo (192 líneas).
- `tools/evolution/tests/test_check_globals_symmetry.py` — 7 tests nuevos.

**Autoría:** coordinador del Plan Omnipotencia, vía directa
(2026-11-16). Sin consejo.
