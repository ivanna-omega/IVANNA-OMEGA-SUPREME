# Motor de Auto-Evolución en Código de IVANNA

> IVANNA mejora su propio código: analiza → propone → escribe el parche →
> valida con tests host → deja listo para revisión → (con aprobación) aplica,
> pushea, la CI compila y publica → rollback si falla.

## 1. Dónde vive el motor (decisión de arquitectura)

**HÍBRIDO. La app nunca reescribe su propio fuente.**

| Capa | Dónde vive | Qué hace |
|---|---|---|
| Recolección | Teléfono (daemon + app) | Telemetría, logs de fallos, métricas DSP, eventos de auto-reparación. La sube por el canal existente (SHM/socket del daemon, o el canal Tailscale+SSH de Muse). |
| Propuesta | VM/servidor (este repo) | `tools/evolution/evolution_engine.py`: lee el estado, construye el paquete de conocimiento, detecta candidatos, genera la propuesta. |
| Generación (LLM) | VM/servidor | El LLM (Gemini vía `IvannaGeminiAgent`, o el analista en el MVP) redacta el parche a partir de `knowledge.json`. |
| Validación | VM/servidor | Compila host (`~/.local/bin/cmake` en `build-tests`), corre el/los tests nuevos y la suite completa (847/847 baseline). |
| Aplicación | VM/servidor + CI | Con aprobación de Esse: commit, push (PAT vía stdin, sin guardarlo), la CI compila Gradle/NDK y publica el release. |
| Rollback | VM/servidor + CI | Si la CI falla o el dispositivo reporta regresión, `git revert` del commit y re-release. |

**Por qué no en el teléfono:** el fuente vive en el repo (VM/GitHub), no en el
teléfono. La app no tiene el código, ni compilador host, ni acceso a GitHub.
Lo que sí puede hacer el teléfono es detectar el síntoma (telemetría) y
proponer el problema; la escritura del parche ocurre donde está el repo.

## 2. Base de conocimiento (qué debe saber el motor)

`tools/evolution/knowledge/knowledge.json` (generado por `--scan`):

- **Estado del repo:** HEAD (últimos 5 commits), árbol sucio, nº de targets ctest.
- **Patrones de referencia:** reglas extraídas del propio código que el motor
  debe respetar al proponer (hoy: `SAF_GUARD (2026-09-17)` en
  `app/src/main/cpp/include/saf_runtime.h :: SAFUpdate`).
- **Candidatos detectados (reglas deterministas):**
  - `saf_without_guards`: funciones SAF sin `std::isfinite`.
  - `dead_functions`: definidas y nunca llamadas en el repo.
  - `untested_top_level_modules`: `.cpp` sin target en `tests/CMakeLists.txt`.
- **Lo que falta añadir (próximas iteraciones):** índice semántico del
  codebase (qué módulo hace qué), historial de fallos del dispositivo
  (logcat + telemetría del daemon), metas/directivas del usuario
  (MEMORY.md), resultados de CI por commit, y el backlog de `docs/`.

## 3. Ciclo de una propuesta

```
scan → knowledge.json → detector → propuesta (parche + justificación)
  → --validate: git apply → cmake build → tests nuevos → ctest completo
  → validation.json → git checkout (revertir) → REPORT.md
  → revisión de Esse → (aprobado) commit + push + CI → release
  → (falla) revert + re-release
```

El entregable de cada propuesta es `tools/evolution/proposals/<id>/`:
`0001-*.patch` (diff aplicable), `REPORT.md` (justificación + evidencia),
`validation.json` (resultado de la validación).

## 4. Gates de seguridad (lo que NUNCA se auto-modifica)

- `app/build.gradle.kts`, `app/src/main/cpp/CMakeLists.txt`
- `app/src/main/java/com/ivanna/omega/security/*` (anti-piratería, IntegrityGate)
- `.github/workflows/*`, `tools/security/*`
- Keystores, claves, PATs, secretos.
- El motor rechaza (`GATE`) cualquier propuesta que toque estos archivos.
- **Aprobación:** por defecto toda propuesta requiere revisión humana de Esse.
  La auto-aplicación sin revisión solo para cambios de bajo riesgo y solo si
  Esse lo autoriza explícitamente (aún no autorizado).

## 5. MVP — propuesta 0001 (completada)

`proposals/0001-saf-phi-infinity-guard/`

- **Hallazgo:** `PhiSAFInfinity` (`app/src/main/cpp/saf_phi_infinity.cpp`,
  compilada en el APK, sin llamadores actuales) no tenía las guardas
  SAF_GUARD que su hermana `SAFUpdate` sí tiene. Repro medido: una sola
  entrada no-finita (NaN/Inf) o G≤0 clavaba el gain en 2.0 (máximo).
- **Fix:** guarda NaN/Inf en las 4 entradas, G degenerada (≤1e-6) y
  denominador degenerado → fail-closed (devuelve el gain actual sin tocar
  el estado); sanitización de G movida ANTES de su uso.
- **Test:** `test_saf_phi_infinity.cpp` (6 tests), ruta válida bit-idéntica
  (1.09091) antes/después.
- **Validación:** ver `validation.json` (tests nuevos 6/6 + suite completa).

## 6. Qué falta para la autonomía completa (actualizado 2026-11-14)

1. ~~**Generación por LLM**~~ — HECHO (motor completo 2026-11-14):
   `tools/evolution/llm_client.py` (OmniRoute, modelo `ivanna-free`, key
   solo en `~/.omniroute/ivanna-app-key`) + `draft_patch()` /
   `--draft` / `--evolve` en `evolution_engine.py`. Ver `PIPELINE.md`.
2. ~~**Validación en scratch**~~ — HECHO: `validate_proposal` ya no toca
   el árbol (git worktree desechable + overlay `git diff HEAD` + copia de
   archivos sin seguir; `tree_untouched` verificado). Propuestas 0002 y
   0003 validadas (0003 en scratch, VERDE).
3. **Telemetría del teléfono → conocimiento:** ingestar logs del daemon y
   métricas DSP como candidatos (el teléfono propone el síntoma).
4. **Índice semántico del codebase:** qué hace cada módulo, para proponer
   mejoras de diseño y no solo guardas.
5. **Auto-aplicación con aprobación de Esse:** el flujo commit/push/CI ya
   existe (lo usa el release 370). El gate de aprobación existe desde
   2026-11-14: pre-push hook (`install_publish_hook.py`, aplicado por
   git fuera del proceso del agente) que exige `.ivanna-publish-approval`
   del operador con proposal+head, rama local main y
   `IVANNA_ALLOW_PUBLISH=1`; sin eso el push a origin/main se bloquea.
6. **Rollback automático en dispositivo:** si la versión instalada reporta
   regresión (telemetría), revertir al release anterior.
7. **Más detectores:** tests que faltan por módulo, duplicación de código,
   funciones con complejidad alta. Añadidos 2026-11-14: `host_checks_red`
   (los 6 chequeos de Fase 0, hoy 6/6 en rojo) y `todo_fixme` (28).
   Pendiente: afinar `dead_functions` (lento, falsos positivos/negativos).

## 7. Uso

```bash
# Solo analizar (no toca el árbol):
python3 tools/evolution/evolution_engine.py --scan

# Ciclo completo de una propuesta (valida en scratch, no aplica):
python3 tools/evolution/evolution_engine.py --proposal 0001 --validate

# Redactar un draft con el LLM (sin validar) y ciclo completo:
python3 tools/evolution/evolution_engine.py --draft 0004 --finding '{...}'
python3 tools/evolution/evolution_engine.py --evolve 0004 --finding '{...}'

# Unit tests del motor:
python3 -m unittest discover -s tools/evolution/tests -v
```

El pipeline completo (detectores, redacción LLM, gates en orden, lecciones)
está en `tools/evolution/PIPELINE.md`.

## 8. Vista de Dios (dónde vive, qué muestra, cómo se alimenta)

La Vista de Dios NO es un dashboard del repo: son dos artefactos web
externos, fuera de `~/workspace/ivanna/` (NO crear otro duplicado):

- **Foto estática** `~/workspace/ts-spaces/ami-vista-de-dios/`
  (`index.html`, 37 KB): foto del estado al 14 nov 2026 17:48 CST.
  Datos de referencia etiquetados con su fecha, nunca presentados
  como en vivo.
- **En vivo** `~/workspace/ts-spaces/ami-vista-de-dios-en-vivo/`
  (TypeScript + `app.db`): sonda cada 5 min (cron
  `ami-vista-de-dios-live-refresh`, `~/workspace/cron.d/minutely/`).
  Su `DATA-PLAN.md` documenta las fuentes probadas: API pública de
  GitHub (releases, workflow runs, commits del repo, sin token),
  probe 401 sin key a OmniRoute (vivo/caído, sin leer la key jamás)
  y probe del túnel localhost.run (URL en settings, rota). El cliente
  re-lee el estado cada 30 s.

Lo que la Vista de Dios NO veía: el interior del motor (propuestas,
veredictos del consejo, bloqueos de contención). La pieza del repo
que la alimenta es esta:

```bash
# Foto del motor como JSON (sin secretos, sin tocar el árbol):
python3 tools/evolution/evolution_engine.py --status [--status-out PATH]
# o directo:
python3 tools/evolution/status_export.py [--out PATH]
```

El JSON trae: rama/HEAD, salud real del gateway (ok, latencia, nº de
modelos, cuáles de los 6 de `ivanna-free` están presentes), roles del
consejo, una fila por propuesta (veredicto medido, consejo, archivos,
`tree_untouched`) y la cola de auditoría (`audit_tail`, 20 líneas).
Verificación medida del gateway y del consejo (modelos, latencias,
coste desconocido): `tools/evolution/OMNIROUTE_VERIFICATION.md`.
El artefacto en vivo no lee este JSON solo todavía: el operador lo
publica/adjunta a la sonda cuando la Vista de Dios lo pida; el
artefacto nunca recibe la API key ni accede al filesystem de la VM.

## 9. Bloque `repro:` obligatorio en cada propuesta (C12, release 510)

Un resultado sin su comando de reproducción no es un resultado. Desde
C12, cada propuesta nueva de `tools/evolution/proposals/<id>/` trae un
sidecar JSON `repro.json` con `command`/`commands`, `seed`, `fixtures`
(rutas repo-relativas con sha256 del fixture REAL) y `expected` (salida
literal que el comando debe producir). El chequeo
`tools/evolution/check_repro.py` (solo stdlib) lo verifica y el job
`repro-check` de `.github/workflows/evolution-council.yml` falla el PR
si el bloque falta, si un fixture no existe o su sha256 no coincide, o
si el comando no produce lo esperado.

**Desviación documentada respecto a la propuesta:** el `repro:` NO es
un bloque de texto en `REPORT.md`. Verificado contra el código vivo
(2026-11-28): las propuestas reales son directorios con
`<id>.patch` + `REPORT.md` + `validation.json`, sin YAML ni
front-matter, y `council.py` nunca lee esos directorios (recibe un
finding JSON por CLI) — no existe ningún campo `repro:` previo que
reutilizar. El sidecar `repro.json` junto a `validation.json` es el
encaje real, con exactamente las claves que pide C12. La forma de un
solo comando (`command`/`seed`/`fixtures`/`expected` a nivel superior)
también se acepta; `commands` (lista) cubre repros de varios pasos.

**Alcance:** solo las propuestas tocadas por el PR (`--changed`). Las
anteriores a C12 (0001-0033) están exentas: exigirlo retroactivamente
pondría la CI en rojo el primer día y rellenarlo a posteriori sería
fabricar evidencia. Los pasos aún no ejecutables van en
`pending_commands` con `blocked_by` obligatorio; si el programa del
paso ya existe, el chequeo falla (tripwire de promoción).

**Ejemplo canónico:** `proposals/0034-v1.1-ab-candidate/repro.json`
(C7+S4, ambos ejecutables; C7 sobre fixtures sintéticos — ver su
REPORT.md). Verificación local:

```bash
python3 tools/evolution/check_repro.py --proposal 0034-v1.1-ab-candidate
python3 tools/evolution/check_repro.py --changed --base <sha-del-base>
python3 -m unittest tools.evolution.tests.test_check_repro -v
```
