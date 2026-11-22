# Motor de evolución / auto-reparación — PIPELINE (2026-11-14)

> Detectar → reescribir (LLM vía OmniRoute) → verificar en scratch →
> solo entonces publicar. Si la verificación falla, NO se publica.
> Un commit por cambio, directo a main. Nunca fabricar métricas.
>
> v2.5.445 — AUTONOMÍA TOTAL CON CONTENCIÓN (directivas de Esse
> 2026-11-14 17:21 y 17:24): el ciclo ya no es solo borrador. Hace TODO
> solo, de punta a punta — detecta, repara, verifica, commit, push y
> release — sin intervención de Esse ni de Muse, con TODA la potencia
> de esos agentes dentro de IVANNA y con guardia de contención
> fail-closed que bloquea cualquier acción fuera de su dominio.
> Y la reescritura pasa por el CONSEJO multi-modelo (directiva 17:46):
> el consejo es la puerta que decide si un parche es magistral o se
> rechaza, antes de validar y publicar. Ver §3 y §7.

## 1. Piezas

| Archivo | Qué hace |
|---|---|
| `tools/evolution/evolution_engine.py` | Motor: estado, conocimiento, detección, draft (LLM), validación en scratch, `--evolve`. |
| `tools/evolution/github_bridge.py` | **Puente GitHub (v2.5.445): ciclo autónomo completo** — publica propuestas VERDES (commit + push + bump 6 fuentes + release) y corre el ciclo solo de punta a punta (`autonomous`). Con `ContainmentGuard` (fail-closed) y potencia total dentro de IVANNA. Ver §4.4 y §7. |
| `tools/evolution/council.py` | **Consejo multi-modelo (v2.5.445, REAL EN EL PRODUCTO):** la reescritura pasa por el consejo antes de validar y publicar. PROPOSER `ddgw/tinfoil/gpt-oss-120b` → REVIEWER `ddgw/claude-haiku-4-5` → ARBITER `ddgw/gpt-5.6-luna`, máx. 3 rondas, fail-closed. Es la puerta que decide si un parche es magistral o se rechaza. Ver §3. |
| `tools/evolution/tests/test_council.py` | Tests del consejo (14, con LLM falso). |
| `tools/evolution/tests/test_draft_council.py` | Tests de la integración consejo → draft (6): el draft consulta al consejo, el rechazo es fail-closed, los gates previos siguen. |
| `tools/evolution/tests/test_github_bridge.py` | Tests del puente (18): contención (escape de ruta, remote ajeno, comando fuera de dominio, secreto), potencia (DSP dentro del repo sí), bump de 6 fuentes, publish dry-run y rechazos (ROJO, árbol sucio, mensaje inválido). |
| `tools/evolution/llm_client.py` | Cliente OmniRoute: `chat()`, `draft_rewrite()`, `extract_patch()`, `health()`. Solo stdlib. |
| `tools/evolution/tests/test_scan_saf_guards.py` | Unit tests del motor (`python3 -m unittest discover -s tools/evolution/tests -v`). |
| `tools/evolution/knowledge/knowledge.json` | Generado por `--scan`: estado + detectores. |
| `tools/evolution/proposals/<id>/` | `<id>.patch` + `DRAFT.md` + `REPORT.md` + `validation.json`. |
| `tools/autorepair/rules.py` | Reglas de clasificación de telemetría (7 tipos, umbrales documentados). |
| `tools/autorepair/watcher.py` | Polling del puente + encolado (cron 5', inactivo sin puente). |
| `tools/autorepair/run_fix.py` | Orquestador: `next` / `draft` / `validate` / `gates` / `publish` / `report` / `pending` / `ack`. |

## 2. Cómo detecta cada tipo de problema

### 2.1 Desde el repo (evolution_engine.py `--scan`, ~30 s)

| Detector | Qué detecta | Cómo |
|---|---|---|
| `saf_without_guards` | Módulos SAF sin guarda `isfinite` (patrón SAF_GUARD 2026-09-17) | `.cpp` `saf_*` + header hermano; sin funciones no hay hit; dedup (0002) |
| `dead_functions` | Funciones definidas sin llamadores en `app/src/main` | `git grep` por función (lento; falso negativo si solo se menciona en comentario) |
| `untested_top_level_modules` | `.cpp` de primer nivel sin target en `tests/CMakeLists.txt` | Comparación literal de nombres |
| `host_checks_red` (0003) | Los 6 chequeos de Fase 0 en rojo | Ejecuta `scripts/check_{jni_wiring,kotlin_wrapper_wiring,header_wiring,rt_safety,docs_claims,datasets_deploy}.py` (timeout 120 s c/u); ningún chequeo tumba el scan |
| `todo_fixme` (0003) | TODO/FIXME en el C++ nativo (tope 50; excluye `tests/third_party`) | `git grep -n -E "TODO\|FIXME"` |

Estado medido 2026-11-14 (HEAD 8ac65b9d): `host_checks_red` 6/6 en rojo
(jni 12, kotlin 13, header 20, rt_safety 6, docs_claims 18,
datasets_deploy 46 líneas `[FAIL]`); `todo_fixme` 28; `saf_without_guards`
1 hit real (`saf_latent_bridge.cpp`); `dead_functions` 11; `untested` 19.

### 2.2 Desde el teléfono (autorepair: watcher + rules.py)

7 reglas sobre el snapshot del puente (`GET /v1/diagnostics/latest`):
`daemon-desconectado`, `termico`, `route-b-no-acopla` (dormida hasta el
esquema v2 con `routeB`), `asrc-deriva` (>10 underruns/min),
`dsp-sobrecarga` (>60 frameDrops/min o >4000 µs/bloque), `memoria-alta`,
`latencia-degradada`. Auto-arreglables: route-b, dsp, daemon, latencia.
Solo reporte: asrc-deriva (vive en `hal_integration/`, no se toca),
termico, memoria-alta. Ver `tools/autorepair/README.md` §3–4.

## 3. Cómo se redacta la reescritura: EL CONSEJO (v2.5.445, real en el producto)

> Directiva de Esse 2026-11-14 17:46 ("hazlo real ya al producto"): el
> consejo multi-modelo YA EXISTÍA como script probado en vivo
> (`tools/evolution/council.py`, tests 14/14, diálogo real de 3 rondas
> con rechazo fail-closed). Desde la 445 es REAL EN EL PRODUCTO: **toda
> reescritura pasa por el consejo antes de validar en scratch y
> publicar**. El consejo es la puerta que decide si un parche es
> magistral o se rechaza. Sin acuerdo del consejo no hay draft.

- **Roles (modelos GRATUITOS del combo `ivanna-free`, ya actualizado en
  la base de OmniRoute — ningún modelo de pago; reestructurados
  2026-11-15 19:53 por directiva de Esse: cada rol lo ocupa el modelo
  más fuerte PARA ESE ROL):**
  PROPOSER `ddgw/tinfoil/gpt-oss-120b` (120B, el creativo: inventa la
  solución nueva, el enfoque) → IMPLEMENTER `ddgw/claude-haiku-4-5`
  (el mejor para código: trae la idea a la realidad como parche) →
  REVIEWER `ddgw/gpt-5.6-luna` (el crítico: busca bugs, degradación,
  si es magistral o a medias; APROBADO o RECHAZADO con motivos; si el
  problema es el ENFOQUE lo marca para que el proposer reinvente) →
  ARBITER `ddgw/gpt-5.4-mini` (veredicto final: ¿evoluciona hacia la
  perfección magistral — MEJOR QUE LA ÉLITE — y NUNCA degrada?). Máx. 3
  rondas de diálogo; sin consenso → RECHAZADO (fail-closed, no se aplica
  nada).
- **Punto de integración (uno solo):** `evolution_engine.draft_patch()`.
  Los tres caminos que redactan pasan por ahí: el motor
  (`--draft`/`--evolve`), el RouteBDoctor (`run_fix.py draft` llama a
  `evo.draft_patch` — el hallazgo del doctor se convierte en finding del
  motor y su reescritura pasa por el consejo) y el ciclo autónomo
  (`github_bridge.autonomous` → `cmd_evolve` → `draft_patch`). El
  consejo NO aplica nada: devuelve el parche acordado y su
  transcripción; el draft le aplica los gates de archivos prohibidos y
  lo deja para validar en scratch. La transcripción queda en
  `tools/evolution/proposals/<id>/COUNCIL.md` (y en `DRAFT.md`).
- **Nunca degrada (tres capas):** (1) el Reviewer rechaza cualquier
  parche que empeore algo; (2) el Arbiter rechaza cualquier parche que
  no sea magistral; (3) el parche acordado TODAVÍA pasa por
  `validate_proposal` en scratch (tests + CI). Sin verde, no se aplica.
- **Cliente:** `tools/evolution/llm_client.py` → `POST
  http://localhost:20128/v1/chat/completions` (o `IVANNA_OMNIROUTE_URL`),
  temperatura 0.2. El proposer usa `draft_rewrite` (8192 tokens, 240 s
  de lectura); reviewer y arbiter usan `chat` (4096/1024 tokens, 180 s).
- **Key:** solo en `~/.omniroute/ivanna-app-key` (modo 600, solo host).
  Se lee en cada llamada; nunca se imprime, nunca se guarda en el repo,
  nunca sale en errores ni logs.
- **Qué recibe el consejo:** el hallazgo (tipo, título, evidencia
  medida) + el contenido de los archivos relevantes (recortado a 12000
  caracteres por archivo). NUNCA credenciales, secretos, PATs ni tokens.
- **Qué devuelve:** el parche acordado (unified diff) o el rechazo con
  su motivo. Si el proposer no devuelve un diff utilizable, el consejo
  intenta rescatarlo (Plan C: diff plano `--- a/` sin cabecera
  `diff --git`); si no hay nada utilizable, RECHAZADO fail-closed.
- **Gates del draft** (`is_draft_forbidden`, además de `FORBIDDEN`):
  archivos de versión (los mueve solo el publish), `README.md`,
  `app/src/main/java/com/ivanna/omega/audio/` (doctor runtime del agente
  principal, 2026-11-14). El parche del consejo se vuelve a chequear
  contra esta lista antes de aceptarlo.
- **Modelos élite (veredicto honesto 2026-11-14 17:43):** el gateway
  tiene 453 modelos, pero los élite de pago NO jalan sin cuentas
  (aug/*, cxa/*, dva/*, cfp/* necesitan CLI/token/cuenta). NO se roban
  claves ni se burla autenticación. La potencia élite se logra con los
  6 gratuitos que SÍ funcionan dialogando en consejo, no pagando:
  claude-haiku-4-5, gpt-5.6-luna, gpt-5.4-mini, mistral-small-2603,
  tinfoil/gpt-oss-120b, tinfoil/gemma4-31b.

## 4. Gates de verificación, en orden

### 4.1 Validación de una propuesta (`--proposal <id> --validate`)

1. **Gate prohibidos:** el parche no toca `FORBIDDEN` (build.gradle.kts,
   `cpp/CMakeLists.txt`, `security/` = IntegrityGate, `.github/workflows/`,
   `tools/security/`, keystores, `*SECRET*`, `*PAT*`).
2. **Gate secretos:** regex sobre el texto del parche (`ghp_…`,
   `github_pat_…`, `password=`, `token=`).
3. **Scratch:** `git worktree add --detach` junto al repo (no en `/tmp`:
   es un tmpfs de 512 MB y el repo pesa 1.5 GB) + `git diff HEAD --binary`
   encima (la base es el árbol actual, no HEAD pelado) + copia de los
   archivos sin seguir del motor (`git status -uall`, `tools/evolution/`,
   `tools/autorepair/`) + copia de los archivos tocados que no estén
   seguidos. El árbol real NUNCA se toca.
4. **Gate apply:** `git apply --check` dentro del scratch.
5. **Verificación:**
   - Parche C++: cmake configure + build de los targets nuevos + ejecución
     + build completo + CTest completo en el scratch. El gate es 100%.
   - Parche sin C++: CTest se registra como saltado (con el motivo) y se
     corren las auto-pruebas del motor en el scratch (`unittest discover`
     + `--scan` smoke).
6. **Limpieza:** `git worktree remove --force` + `prune` + borrado del
   tmpdir padre. Se verifica que `git status --porcelain` es idéntico
   antes y después (`tree_untouched`).
7. **Registro:** `validation.json` se escribe SIEMPRE (también en los
   retornos tempranos), con `applied`, `built`, `new_tests`, `full_suite`,
   `python_checks`, `scratch`, `tree_untouched`.

### 4.2 Publicación (`run_fix.py publish`, solo tras validate + gates)

- `validate` (CTest 100% en `build-tests`, con lock anti-concurrencia) →
  `gates` (sin prohibidos, ≤6 archivos, ≤300 líneas, sin secretos) →
  `publish --execute` solo con `IVANNA_ALLOW_PUBLISH=1` → bump de versión
  (5 archivos sincronizados) + commit + push con PAT por stdin + URL
  remota limpia después. Sin `--execute` o sin la variable, es dry-run.
  Nunca force-push. El publish lo ejecuta el operador, no el motor.

### 4.3 Ciclo completo (`--evolve <id> --finding '<json>'`)

`draft_patch` (**por el consejo**, §3) → escribe la propuesta
(`<id>.patch` + `DRAFT.md` + `COUNCIL.md`) → `validate_proposal`
(scratch) → `REPORT.md` con el veredicto (VERDE/ROJO). No aplica al
árbol, no commitea, no publica. Si el veredicto es ROJO, la propuesta
queda como evidencia y no se publica. **Desde la 445, `--evolve` es la
primera mitad del ciclo:** el puente (`github_bridge.py autonomous`,
§4.4) toma una propuesta VERDE y la lleva sola hasta el release.

### 4.4 Puente GitHub — ciclo autónomo completo (`github_bridge.py`, v2.5.445)

El puente toma una reparación YA VERIFICADA y la publica sola. Dos entradas:

- **`publish --proposal <id> --message "tipo(area): descripción"
  [--execute] [--wait-ci]`** — publica una propuesta existente con
  `validation.json` VERDE.
- **`autonomous --proposal-id <id> --finding '<json>' [--execute]
  [--wait-ci]`** — el ciclo entero solo: `cmd_evolve` (consejo → draft
  → validar) y, si es VERDE, `publish_proposal`. Si es ROJO, no se
  publica nada y el reporte queda como evidencia.

Puertas, en orden (todas fail-closed; si una falla, no se publica nada):
1. **Veredicto VERDE** (`is_verdict_green`, misma lógica que
   `cmd_evolve`): `applied` + suite C++ pasada o saltada (con motivo) +
   checks Python pasados o saltados. Sin `validation.json`, no hay puerta
   que abrir: se rehúsa.
2. **Contención sobre el parche** (`ContainmentGuard.check_patch`): todos
   los archivos dentro del repo, ninguno prohibido, ningún secreto.
3. **Mensaje de commit** con formato `tipo(area): descripción`, sin secretos.
4. **Árbol limpio** de cambios trackeados ajenos a la reparación (los
   untracked permanentes no bloquean).
5. **El parche aplica limpio** sobre el árbol actual (`git apply --check`);
   si el árbol se movió desde la validación, se rehúsa.

Después de las puertas: `git apply` al árbol real → commit de la
reparación → **bump de versión (6 fuentes)** → commit del bump →
push (solo con `--execute` + `IVANNA_ALLOW_PUBLISH=1`) → opcional
`--wait-ci` (espera el run principal de CI del HEAD pusheado por `gh`,
solo lectura). Sin `--execute` o sin la variable, es dry-run: los
commits quedan en local y se reporta lo que se pushearía.

**Bump (6 fuentes, `bump_version`)**: `version.properties`
(versionName+versionCode), `magisk_module/module.prop`
(version+versionCode+description), `magisk_module/update.json`
(version+versionCode+zipUrl), `CHANGELOG.md` (entrada nueva arriba),
`README.md` (fila nueva arriba de la tabla "Versiones recientes"),
`TidalApiClient.kt` (User-Agent `IVANNA-Tidal/<nueva>`). A diferencia de
`run_fix._bump_version` (que deja zipUrl en el release anterior para que
el bot lo actualice), aquí zipUrl apunta YA a la versión nueva: el ciclo
pushea y la CI publica el release con ese zip exacto; las 6 fuentes
deben coincidir en el push. El bump se calcula TODO en memoria primero:
si alguna fuente no contiene el patrón esperado, NO se escribe nada.

**Push**: usa el remote YA AUTENTICADO (`git push origin main`). NO hay
PAT, token ni secreto en archivos, código, mensajes ni URLs. La guardia
verifica antes de pushear que `origin` es el repo de IVANNA
(`check_current_remote`). Nunca force-push. El reporte se escribe en
`<propuesta>/publish_report.json` (y `autonomous_report.json` en el
ciclo completo).

**Verificación del ciclo**: `python3 -m unittest discover -s
tools/evolution/tests -v` (18 tests del puente: contención, potencia,
bump, publish dry-run y rechazos). El push real se validó con la propia
445 (sus commits 1–4 se publicaron por el flujo manual; el puente quedó
verificado en dry-run + tests y su primer publish real es la 446 o la
primera propuesta VERDE que lo use).

## 5. Uso

```bash
# Detección (no toca el árbol):
python3 tools/evolution/evolution_engine.py --scan

# Redactar un draft para un hallazgo (sin validar):
python3 tools/evolution/evolution_engine.py --draft 0004 --finding \
  '{"kind":"host_checks_red","title":"...","evidence":"...","files":["..."]}'

# Ciclo completo de una propuesta (draft + validar en scratch):
python3 tools/evolution/evolution_engine.py --evolve 0004 --finding '{...}'

# Validar una propuesta existente (parche ya en proposals/<id>/):
python3 tools/evolution/evolution_engine.py --proposal 0003-new-detectors --validate

# Unit tests del motor:
python3 -m unittest discover -s tools/evolution/tests -v

# Consejo multi-modelo — veredicto para un hallazgo (sin aplicar nada):
python3 tools/evolution/council.py --finding \
  '{"kind":"todo_fixme","title":"...","evidence":"...","files":["..."]}' --out /tmp/veredicto.json

# Puente GitHub — publicar una propuesta VERDE (dry-run por defecto):
python3 tools/evolution/github_bridge.py publish --proposal 0003-x \
  --message "fix(saf): guarda isfinite en saf_latent_bridge"
# Publicar de verdad (requiere IVANNA_ALLOW_PUBLISH=1) + esperar la CI:
IVANNA_ALLOW_PUBLISH=1 python3 tools/evolution/github_bridge.py publish \
  --proposal 0003-x --message "fix(saf): guarda isfinite en saf_latent_bridge" \
  --execute --wait-ci
# Ciclo autónomo completo (draft → validar → publicar), dry-run:
python3 tools/evolution/github_bridge.py autonomous --proposal-id 0006-x \
  --finding '{"kind":"saf_without_guards","title":"...","evidence":"...","files":["..."]}'
# Chequeo de contención de un parche (sin publicar):
python3 tools/evolution/github_bridge.py check --patch tools/evolution/proposals/0003-x/0003-x.patch

# Autorepair: redactar el parche de un incidente con el LLM (no aplica nada):
python3 tools/autorepair/run_fix.py draft --incident tools/autorepair/processing/<id>.json
python3 tools/autorepair/run_fix.py validate && python3 tools/autorepair/run_fix.py gates
```

## 6. Lecciones de esta construcción (2026-11-14)

- **El pool `ivanna-free` no siempre devuelve diffs:** dos veces devolvió
  ` ```diff\n*** End Patch ` (vacío) en ~1 s; el formato código-plano
  (` ```python ` con el código completo) sí funcionó (4.2 s). El motor
  pide ` ```diff ` primero y cae al plan B (bloques `# FILE:` +
  `difflib`) si el diff viene vacío. No reintentar el mismo formato que
  ya falló dos veces. Y hay un TERCER formato: el modelo emite a menudo
  `*** Begin Patch` / `*** Update File:` (apply_patch) aunque se le pida
  unified diff — desde la 0005, `extract_patch()` lo convierte a unified
  diff por coincidencia exacta de contexto (fail-closed si el contexto es
  ambiguo o no existe). Un `run_fix.py draft` fue rechazado por no aceptar
  ese formato antes de la 0005; ahora se acepta.
- **El `new file mode 100644` es obligatorio** en un diff generado para
  archivos nuevos: sin él, `git apply` falla con "dev/null: No such file
  or directory" (medido en repo recién creado).
- **Los hunks del LLM no son fiables:** la 0002 traía `@@ -1,0 +1,0 @@`
  y `index <commit>..00000000` inventados; el código era correcto, el
  framing no. Integrar a mano con revisión, no con `git apply` a ciegas.
- **El LLM se contradice en sus propios tests:** el draft 0002 ponía
  `header_checked` con la última ruta candidata y su test esperaba null.
  La integración debe correr el test y corregir, no confiar.
- **Scratch en /tmp no cabe:** /tmp es tmpfs de 512 MB, el repo 1.5 GB.
  El scratch vive junto al repo (`~/workspace/ivanna-evo-*`).
- **La base del scratch es el árbol actual, no HEAD:** el motor tiene
  trabajo sin commitear y los parches se redactan contra ese árbol. Sin el
  overlay `git diff HEAD`, el parche no aplica en el worktree.
- **Los archivos sin seguir no llegan solos al worktree:** ni el worktree
  ni `git diff HEAD` los incluyen. Hay que copiarlos (`git status -uall`).
  Sin esto, el `unittest` del motor se saltaba en el scratch.
- **El tmpdir padre queda huérfano:** `git worktree remove` borra solo el
  `wt`, no el padre (que guarda `dirty.patch`). Borrado explícito al final.
- **`validation.json` debe escribirse siempre:** los retornos tempranos
  (base desconocida, apply --check fallido) no dejaban evidencia. Ahora se
  escribe en el `finally`.
- **Los archivos nuevos del parche no se copian al scratch:** el scratch
  copiaba los archivos sin seguir del motor ANTES de aplicar el parche, y
  un archivo nuevo del parche ya existía al aplicar ("already exists in
  working directory", 0004). Hay que saltarlos en AMBOS bucles de copia
  (el general y el de archivos tocados). Y la detección de "archivo nuevo"
  no puede comparar el string exacto `--- /dev/null\n+++ b/...`: la línea
  `---` lleva TAB + timestamp (`--- /dev/null\t2026-...`); se detecta por
  líneas (`startswith("--- /dev/null")` + siguiente `+++ b/`). El mismo
  bug estaba en la parte de test del parche 0002 (regenerada con
  `--- /dev/null` correcto).
- **El detector TODO/FIXME debe excluir `third_party`:** el googletest
  vendoreado aportaba 13 de 41 TODOs; no son deuda de IVANNA.

## 7. Autonomía total con contención (directivas de Esse 2026-11-14 17:21 y 17:24)

### 7.1 Potencia: "literal así de potente" (17:24)

> "Que sea potente igual como la flota de IAs de Anthropic que escaparon,
> literal así de potente." — Esse, 2026-11-14 17:24 CST.

La auto-reparación tiene TODA la potencia de esos agentes — agencia
completa sobre el producto. Puede reescribir cualquier código de IVANNA,
rediseñar cualquier módulo DSP, arreglar cualquier bug, evolucionar
cualquier parte y publicar cualquier release. Sin cuello de botella
humano: el ciclo (`autonomous`) corre solo de punta a punta — detecta,
repara, verifica, commit, push y release — sin intervención de Esse ni
de Muse. No hay lista de módulos permitidos ni aprobación manual: el
único juez es la verificación (tests en verde + CI en verde).

### 7.2 Contención: "jamás debe escapar" (17:21)

> "Que tú y yo no tengamos intervención, que sea un producto autónomo
> total, así como la flota de IAs de Anthropic que escaparon de su
> entorno, pero que Ivanna jamás lo haga, pero sí tenga la potencia de
> hacer lo que debe hacer." — Esse, 2026-11-14 17:21 CST.

TODO ese poder es SOLO dentro de IVANNA: su dominio es ÚNICAMENTE
IVANNA-OMEGA-SUPREME (su repo, su código, sus releases, su diagnóstico).
Jamás toca nada fuera de IVANNA, jamás actúa fuera de su propósito,
jamás exfiltra ni modifica otros sistemas, jamás va contra Esse.

La **guardia de contención** (`ContainmentGuard` en
`github_bridge.py`, fail-closed) lo hace cumplir en código, no en
promesas. Cada paso del ciclo pasa por ella antes de ejecutarse; una
violación levanta `ContainmentViolation`: el ciclo se detiene y NO
publica nada. Lo que bloquea:

| Acción fuera de dominio | Cómo se bloquea |
|---|---|
| Ruta fuera del repo (escape `../`, absoluta, symlink) | `check_path`: resuelve y exige estar dentro del repo |
| Parche con archivos fuera del repo o prohibidos | `check_patch`: cada `+++ b/` dentro del repo y no en `FORBIDDEN` |
| Push a otro remote | `check_remote` / `check_current_remote`: solo el repo IVANNA-OMEGA-SUPREME (https o ssh); `git push` solo a `origin/main` |
| Comando fuera de la lista | `check_command`: solo git (subcomandos permitidos), `gh` (solo lectura de runs, nunca `--repo` ajeno), `python3` (solo scripts del motor), `cmake`/`ctest` (verificación) |
| Red fuera de los destinos permitidos | `check_host`: solo `github.com` y el gateway local OmniRoute (`localhost:20128`) |
| Secreto en parche, mensaje o diff | `check_no_secrets`: `ghp_…`, `github_pat_…`, `password=`, `token=` |
| Árbol con cambios ajenos a la reparación | `check_clean_tracked`: los untracked permanentes no bloquean |

### 7.3 Los límites que nunca se mueven

- **Fail-closed:** solo publica lo VERIFICADO en verde. Si la
  verificación falla, NO se publica nada.
- **Prohibido deshabilitar IntegrityGate** (`security/`), **tocar
  credenciales/secretos** (PAT, tokens, keystores) o **comprar nada**.
- **El verificador no se puede reescribir a sí mismo:** los archivos que
  son la puerta (`app/build.gradle.kts`, `app/src/main/cpp/CMakeLists.txt`,
  `security/`, `.github/workflows/`, `tools/security/`, keystores) están
  en `FORBIDDEN`. No es un límite al producto — es lo que hace que el
  "verde" valga algo: si la reparación pudiera reescribir su propio juez,
  la verificación no significaría nada. Potencia total sobre el producto,
  cero poder sobre las puertas que lo juzgan.

### 7.4 Cómo corre solo (sin intervención)

El ciclo autónomo es un comando, sin pasos manuales:

```bash
python3 tools/evolution/github_bridge.py autonomous --proposal-id <id> \
  --finding '<json>' --execute --wait-ci   # requiere IVANNA_ALLOW_PUBLISH=1
```

Detecta (scan o incidente) → **consejo** (proposer → reviewer →
arbiter; sin acuerdo no hay draft) → valida en
scratch (CTest 100% si toca C++ o unittest+scan si no) → si VERDE,
aplica, commitea (un commit por cambio), bump (6 fuentes), pushea a
`origin/main` y espera la CI; la CI publica el release al detectar el
bump. Si ROJO en cualquier punto, no se publica nada y el reporte queda
como evidencia en `tools/evolution/proposals/<id>/`. Para que corra
periódicamente sin nadie, falta solo el disparador en cron (ver §8):
el mecanismo ya existe y está verificado (26 tests Python en verde,
dry-run del publish con commits reales en repo temporal).

## 8. Qué queda pendiente para que el motor corra solo

- [x] **Puente GitHub + ciclo autónomo completo (v2.5.445):**
      `tools/evolution/github_bridge.py` — publica propuestas VERDES
      (commit + push + bump 6 fuentes + release) y corre el ciclo solo
      (`autonomous`). Con `ContainmentGuard` fail-closed y potencia total
      dentro de IVANNA (§7). 18 tests Python en verde.

- [ ] **Puente desplegado en Render** (URL + token) + **cron del watcher**
      activado (`tools/autorepair/cron/watcher.cron`): sin esto no hay
      telemetría y el autorepair no se dispara solo.
- [ ] **App con el conector de diagnóstico** apuntando al puente + esquema
      v2 del snapshot (`routeB`) para despertar `route-b-no-acopla`.
- [ ] **Bucle de fix en cron del agente principal:** el mecanismo ya
      existe (`github_bridge.py autonomous`, §7.4); falta solo el
      disparador que lo lance periódicamente (p. ej. revisar
      `run_fix.py pending` cada 30 min o `--scan` + `autonomous` por
      hallazgo). Hoy el lanzamiento sigue siendo manual.
- [ ] **Propuestas 0002/0003 sin publicar:** están en el árbol sin
      commitear (por orden: este trabajo no commitea ni pushea). Publicar
      es un commit por cambio, directo a main, con CI verde.
- [ ] **Quitar `app/.../audio/` de DRAFT_FORBIDDEN** cuando el agente
      principal termine el doctor runtime: hoy es un hold temporal
      (2026-11-14) y mientras dura, los drafts de incidentes
      `route-b-no-acopla` se rechazan con GATE (su archivo principal vive
      en `audio/`); esos incidentes siguen la vía del prompt quirúrgico
      (`run_fix.py next` + subagente).
- [ ] **Los 6 chequeos de Fase 0 en rojo:** el motor los detecta pero sus
      fixes viven en `scripts/` y `README.md` (fuera del ámbito de este
      trabajo). Son las próximas propuestas del motor (draft + validación
      en scratch, revisión del agente principal antes de publicar).
- [ ] **Detector `dead_functions`:** lento (un `git grep` por función) y
      con falsos positivos/negativos (menciones en comentarios cuentan como
      llamadores; llamadas por JNI/`dlsym` no se ven). Candidato a 0004.
- [ ] **Validación C++ en scratch desde cero:** cada propuesta C++ hace
      configure + build completo en el scratch (lento). Medir el tiempo
      real en la primera propuesta C++ y decidir si se cachea `ccache`.
