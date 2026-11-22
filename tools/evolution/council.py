#!/usr/bin/env python3
"""
Consejo multi-modelo de IVANNA — tools/evolution/council.py.

Las IAs NO trabajan solas: dialogan entre ellas, se critican y solo
aplican a IVANNA lo que el consejo acuerda. Cada modelo tiene un rol y
una voz distinta; la reescritura que llega a IVANNA es el acuerdo del
consejo, no la ocurrencia de un solo modelo.

Roles (modelos GRATUITOS que sí funcionan vía OmniRoute).
REGLA DE ORO DE ESSE (2026-11-15 19:53 CST): cada rol lo ocupa el modelo
más fuerte PARA ESE ROL — el creativo va al más creativo, capaz de
inventar cosas nuevas; el trabajo de traer a la realidad va a los
mejores para código. No queremos perfecto estándar: queremos perfecto
mejor que los élite, mejor que la tecnología existente.
REFORMULACIÓN (2026-11-16 02:00 CST, directiva de Esse): cada rol se
asigna por MEDICIÓN, no por nombre — benchmark /tmp/bench_council.py
(CREATE/CODE/CRITIQUE/DETECT) sobre los modelos disponibles. Los pro de
Gemini (4-argon, 3.1-pro-preview) están en cooldown de 24 h hasta
2026-11-18 ~02:00 CST (429 model_cooldown, retry_after 2026-11-18T07:59:58Z,
probado en directo 2026-11-17 12:22 CST; el cooldown NO se liberó el
2026-11-17 ~02:00 CST como se esperaba), así que la tabla de Esse
(2026-11-16 02:09 CST) se aplica con dos roles INTERINOS hasta el
2026-11-18 ~02:00 CST.
Roles v2.0 — REORGANIZACIÓN 2026-11-17 13:34 CST (propuesta traída por
Esse, adoptada: Consejo Nuclear de 6 + Caballería Pro). Antes: 14 sillas,
2 implementers rotos, Luna relegada a reviewer, haiku vetando mejoras
como falso positivo, gemma inventando APIs, 2 modelos muertos (404), 1
mudo, 1 timeout, 3 en 429. Resultado: 0035 y 0036 cerraron con sustancia
aprobada y parche aplicable NINGUNO. Ahora:
  - DETECTOR DE CÓDIGO REAL  ddgw/mistral-small-2603 (titular; confirmó
    en código max(), 4 escritores, compresor monobanda, daemon 20x/s).
  - VERIFICADOR EMPÍRICO  gemini/gemini-3.1-flash-lite (cada cifra debe
    tener fuente: medición, log o código; si no, SOSPECHOSA).
  - ARQUITECTO JEFE Y PRESCRIPTOR  ddgw/gpt-5.6-luna (promovido: su
    prescripción de 5 puntos en 0035 fue el único producto usable; en
    cambios <120 líneas genera él mismo el parche de referencia).
  - IMPLEMENTER ENJAULADO  ddgw/tinfoil/gpt-oss-120b (sin rol proposer;
    mundo cerrado ALLOWED_SYMBOLS; si falta un símbolo responde
    MISSING_SYMBOL en vez de inventar; gate mecánico antes del arbiter).
  - IMPLEMENTER RESPALDO  ddgw/tinfoil/gemma4-31b (0045: CODE 2/2
    aplicable multi-archivo vs titular 0/2; entra en los intentos 3-5
    y en el reintento del gate, mismo sistema enjaulado y mismo gate).
  - CENTINELA ANTI-REGRESIÓN  ddgw/claude-haiku-4-5 (reasignado:
    prohibido vetar mejoras como falso positivo y prohibido pedirle
    diffs; solo busca regresiones en un parche que ya aplica y compila).
  - CONTRAPESO (reviewer2)  gemini/gemini-3-flash-preview (0045:
    CRITIQUE 1.0 + DETECT 1.0; responde en 2.4-12.8 s; la cuarentena por
    timeout 107 s en 0035 NO se reproduce).
  - ARBITER SUPREMO  ddgw/gpt-5.4-mini (DOBLE VEREDICTO: sustancia vs
    ejecución; RECHAZO PARCIAL = rehacer el diff sobre el plano aprobado,
    el frente no muere).
  - CABALLERÍA PRO (relevo 1-a-1, nunca apilados; tras el reset 429 del
    2026-11-18 ~02:00 CST): gemini-4-argon releva a luna (co-arquitecto
    DSP), gemini-3.1-pro-preview releva a mistral (auditor forense),
    gemini-3.7-flash releva a haiku (revisor técnico rápido).
  - CUARENTENA: gemini-3.8-flash (null x3 en 0035; timeout 273 s en
    0045). PURGA: gemini-2.5-pro, gemini-2.5-flash-lite (404),
    gemini-2.5-flash (404 en 0045: "no longer available"). RESERVA FRÍA:
    ninguna (gemma4-31b pasó a implementer_backup en 0045).

Pipeline v2.0 (máx. 2 rondas, 4 fases rígidas):
  Fase 0 — TIERRA FIRME (local, 0 llamadas al consejo): el Coordinador
    extrae archivos exactos, fragmentos completos de los métodos a tocar,
    tabla ALLOWED_SYMBOLS, evidencia de teléfono/Esse y etiqueta
    [MEJORA/DSP], [BUG] o [ARQUITECTURA]. Sin Tierra Firme no se convoca.
  Fase 1 — DIAGNÓSTICO + PLANO (1 ráfaga): Mistral + Flash-Lite validan
    código y números en paralelo; Luna escribe la Prescripción Exacta (y
    en cambios <120 líneas, el bloque de código de referencia).
  Fase 2 — IMPLEMENTACIÓN + GATE MECÁNICO (local): el Implementer emite
    el diff; el Coordinador valida mecánicamente (git apply --check sobre
    el árbol real; símbolos contra ALLOWED_SYMBOLS). Si falla: rebote
    directo al Implementer (máx. 1 reintento). Sin aplicar, no pasa a
    revisión ni al Árbitro.
  Fase 3 — BLINDAJE + VEREDICTO (1 ráfaga): Haiku audita regresiones
    sobre un parche que ya aplica y compila; Mini emite el doble veredicto.
  Sin consenso en 2 rondas → RECHAZADO (fail-closed, no se aplica nada);
  sustancia aprobada + ejecución rechazada → RECHAZO PARCIAL (el frente
  no muere: se rehace el diff sobre el plano aprobado).

NUNCA DEGRADAR (fail-closed, tres capas):
  - Capa 1: el Reviewer rechaza cualquier parche que empeore algo.
  - Capa 2: el Arbiter rechaza cualquier parche que no sea magistral.
  - Capa 3: el parche acordado TODAVÍA pasa por evolution_engine
    .validate_proposal (scratch + tests + CI). Sin verde, no se aplica.
  El consejo NO aplica nada al árbol: devuelve el parche acordado y su
  transcripción; el llamador valida y decide.

Barreras de pago (honesto, 2026-11-14):
  Los modelos élite de pago (Opus, GPT-5 Pro, Gemini 3 Pro vía aug/cxa/dva)
  exigen cuentas de pago (Augment, Codex, Devin) que NO se pueden
  desbloquear sin esas cuentas — no se roban claves ni se burla
  autenticación. El consejo logra nivel élite con modelos GRATUITOS
  colaborando: un 120B que inventa y forja (proposer + implementer
  interinos) + un GPT-5.6 que critica + un GPT-5.4-mini que arbitra + un
  Claude que detecta (distingue lo real del ruido) + un Gemini de
  contrapeso superan a un solo modelo caro sin revisión. La potencia está
  en el diálogo, no en pagar. Los pro de Gemini (4-argon, 3.1-pro-preview)
  están en cooldown de 24 h hasta 2026-11-18 ~02:00 CST; cuando se
  liberen, proposer pasa a 4-argon y detector a 3.1-pro-preview.

Uso:
  from council import council_review
  verdict = council_review(finding, code_context)
  # verdict: {"approved": bool, "patch": str|None, "rounds": [...],
  #           "reviewer": {...}, "arbiter": {...}, "reason": str}

  python3 tools/evolution/council.py --finding '{"kind":"...","title":"...","evidence":"...","files":[...]}'
"""
import json
import re
import sys
from pathlib import Path

EVO = Path(__file__).resolve().parent
REPO = EVO.parents[1]

# Roles del consejo v2.0 — REORGANIZACIÓN 2026-11-17 13:34 CST (propuesta
# traída por Esse, adoptada: Consejo Nuclear de 6 + Caballería Pro).
# Antes: 14 sillas, 2 implementers rotos, Luna relegada a reviewer, haiku
# vetando mejoras como falso positivo, gemma inventando APIs, 2 modelos
# muertos (404), 1 mudo, 1 timeout, 3 en 429. Resultado: 0035 y 0036
# cerraron con sustancia aprobada y parche aplicable NINGUNO.
# Ahora: mistral detecta en código real; luna es ARQUITECTO JEFE (su
# prescripción de 5 puntos en 0035 fue el único producto usable); el
# implementer va ENJAULADO (mundo cerrado ALLOWED_SYMBOLS + gate mecánico);
# haiku es CENTINELA anti-regresión (sin veto, sin diffs); mini arbitra
# con DOBLE VEREDICTO (sustancia vs ejecución; rechazo parcial = rehacer
# el diff sobre el plano aprobado, el frente no muere).
# CABALLERÍA PRO (relevo 1-a-1, nunca apilados, tras el reset 429 del
# 2026-11-18 ~02:00 CST): argon releva a luna (co-arquitecto DSP),
# 3.1-pro-preview releva a mistral (auditor forense), 3.7-flash releva a
# haiku (revisor técnico rápido).
# CUARENTENA: gemini-3.8-flash (null x3 en 0035; timeout 273 s en 0045).
# PURGA: gemini-2.5-pro, gemini-2.5-flash-lite y gemini-2.5-flash (404).
# 0045 (2026-11-17 22:24 CST, orden de Esse: potenciar el consejo):
# reviewer2 = gemini-3-flash-preview (CRITIQUE 1.0 + DETECT 1.0 medidos;
# responde 2.4-12.8 s; timeout 107 s de 0035 no se reproduce) e
# implementer_backup = gemma4-31b (CODE 2/2 aplicable vs titular 0/2;
# entra intentos 3-5 y reintento del gate). Fix _extract_plain_diff:
# cabecera 'diff --git' por archivo (un diff plano multi-archivo salía
# corrupto; medido: luna/gemma escriben el cambio correcto sin cabeceras).
ROLES = {
    "detector": "ddgw/mistral-small-2603",  # DETECTOR DE CÓDIGO REAL (titular)
    "verifier": "gemini/gemini-3.1-flash-lite",  # VERIFICADOR EMPÍRICO
    "proposer": "ddgw/gpt-5.6-luna",  # ARQUITECTO JEFE Y PRESCRIPTOR
    "implementer": "ddgw/tinfoil/gpt-oss-120b",  # IMPLEMENTER ENJAULADO
    # RESPALDO (0045, medido /tmp/bench_council.py CODE 2/2 aplicable vs
    # titular 0/2): entra en los intentos 3-5 y en el reintento del gate.
    "implementer_backup": "ddgw/tinfoil/gemma4-31b",
    "reviewer": "ddgw/claude-haiku-4-5",  # CENTINELA ANTI-REGRESIÓN
    # CONTRAPESO (0045, medido CRITIQUE 1.0 + DETECT 1.0; responde en
    # 2.4-12.8 s; la cuarentena por timeout 107 s en 0035 NO se reproduce).
    "reviewer2": "gemini/gemini-3-flash-preview",
    "arbiter": "ddgw/gpt-5.4-mini",  # ÁRBITRO SUPREMO (doble veredicto)
}
# Pro (cooldown hasta 2026-11-18 ~02:00 CST): proposer pro gemini-4-argon
# (releva a luna), detector pro gemini-3.1-pro-preview (releva a mistral).

MAX_ROUNDS = 2
# Presupuesto TOTAL de contexto del consejo (2026-11-16, prueba de fuego;
# CORREGIDO 2026-11-17 por el seguimiento 0035): el proveedor ddgw
# (DuckDuckGo) rechaza entradas >~13k caracteres con ERR_INPUT_LIMIT
# (HTTP 429) — el brief completo del seguimiento murió 2 veces; el
# presupuesto seguro por llamada es ~13000 caracteres TOTALES (system +
# user), NO 16000. El driver council_driver_0035seg.py baja el contexto a
# 4,500 chars en runtime y loguea cada llamada.
# El brief = system (~1.2k) + contexto + envoltura (~1.5k); con 10k de
# contexto el total queda ~13k, con margen. Antes cada archivo se
# recortaba a 12000 SIN tope total: un hallazgo con 4 archivos sumaba
# ~19k y el proposer fallaba antes de inventar nada.
MAX_CONTEXT_CHARS = 10000
# Tope de la solución del proposer que se pasa al implementer: el
# implementer recibe system (~1.1k) + brief (~11.5k) + solución +
# feedback del reviewer en rondas 2-3; el límite de entrada del
# proveedor es ~16k totales. El tope es DINÁMICO: se recorta la solución
# a lo que quepa tras restar system, brief y feedback (en la ronda 2 de
# la prueba de fuego el feedback empujó la llamada a ~17k y el
# implementer devolvió un diff vacío de 21 chars). La cola que se
# pierde es RIESGOS, lo menos crítico para implementar.
MAX_INPUT_CHARS = 13000
PROPOSER_MAX_TOKENS = 1000
# Tope del parche del implementer: un parche de 300 líneas son ~12k
# chars; con 3072 tokens el reviewer (system ~1k + envoltorio ~2.6k +
# parche) queda ~15.6k como máximo, dentro del límite.
IMPLEMENTER_MAX_TOKENS = 3072


def _keywords_from_finding(finding: dict):
    """Símbolos del hallazgo (identificadores snake_case/CamelCase de
    título+evidencia) para centrar la ventana de contexto en lo relevante."""
    text = f"{finding.get('kind', '')} {finding.get('title', '')} {finding.get('evidence', '')}"
    kws = []
    for tok in re.findall(r"[A-Za-z_][A-Za-z0-9_]{3,}", text):
        if tok not in kws and ("_" in tok or any(c.isupper() for c in tok[1:])):
            kws.append(tok)
    return kws


def _relevant_window(src: str, keywords, budget: int) -> str:
    """Ventana de `budget` chars centrada en el primer símbolo relevante
    del hallazgo; si no hay ninguno, los primeros chars. Así el parcheador
    ve la función que debe tocar (p. ej. scan_saf_guards en la línea 325
    de evolution_engine.py) en vez de las primeras 200 líneas inútiles."""
    best = None
    for kw in keywords or []:
        i = src.find(kw)
        if i != -1 and (best is None or i < best):
            best = i
    if best is None:
        return src[:budget] + "\n[... recortado: supera el presupuesto de contexto ...]\n"
    start = max(0, best - budget // 4)
    end = min(len(src), start + budget)
    head = "[... recortado: inicio del archivo ...]\n" if start > 0 else ""
    tail = "\n[... recortado: resto del archivo ...]" if end < len(src) else ""
    return head + src[start:end] + tail
# Tiempo de lectura por llamada: el gateway es lento (medido 37 s para un
# "OK" trivial). El draft necesita 240 s; la revisión, 180 s.
REVIEW_READ_TIMEOUT_S = 180
DRAFT_READ_TIMEOUT_S = 240

# Veredicto del revisor/árbitro: primera línea APROBADO o RECHAZADO.
_VERDICT_RE = re.compile(r"^\s*(APROBADO|RECHAZADO)\b", re.I | re.M)


def _llm():
    """Import perezoso de llm_client (mismo directorio)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("ivanna_llm_client", EVO / "llm_client.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _parse_verdict(text: str):
    """Extrae (aprobado: bool|None, motivos: str) del texto del revisor.

    El revisor debe abrir con APROBADO o RECHAZADO. Si no lo hace, el
    veredicto es None (ambiguo = fail-closed, se trata como rechazo)."""
    if not text:
        return None, "respuesta vacía"
    m = _VERDICT_RE.search(text)
    if not m:
        return None, "sin veredicto explícito (ni APROBADO ni RECHAZADO)"
    return m.group(1).upper() == "APROBADO", text.strip()


def _extract_plain_diff(text: str):
    """Plan C: extrae un unified diff plano (sin cabecera 'diff --git').

    El proposer a veces devuelve un diff válido que empieza por '--- a/'
    (sin la línea 'diff --git'): extract_patch lo rechaza y el consejo
    lo daba por 'sin diff utilizable'. Aquí se acepta si tiene la
    estructura mínima (--- / +++ / al menos un hunk @@). La validez REAL
    (que el contexto exista en el archivo) la verifica después
    validate_proposal con `git apply --check`; aquí solo se exige forma.
    Devuelve el diff o None."""
    if not text:
        return None
    # Quitar la valla ```diff si la hay y quedarse con el bloque.
    m = re.search(r"```(?:diff|patch)\n(.*?)```", text, re.S)
    body = m.group(1) if m else text
    lines = body.strip("\n").splitlines()
    # El diff empieza en la primera línea '--- ' (o 'diff --git').
    start = next((i for i, l in enumerate(lines)
                  if l.startswith("--- ") or l.startswith("diff --git")), None)
    if start is None:
        return None
    diff_lines = lines[start:]
    has_old = any(l.startswith("--- ") for l in diff_lines)
    has_new = any(l.startswith("+++ ") for l in diff_lines)
    has_hunk = any(l.startswith("@@") for l in diff_lines)
    if not (has_old and has_new and has_hunk):
        return None
    # Si falta la cabecera 'diff --git', se reconstruye desde '--- a/X'
    # ANTES DE CADA ARCHIVO (0045: la versión anterior solo la ponía en
    # el primer archivo y un diff plano multi-archivo salía corrupto en
    # git apply — medido /tmp/bench_council.py: luna y gemma escriben el
    # cambio correcto en 2 archivos pero sin cabeceras, y el gate lo
    # rechazaba como "corrupt patch"). Si la cabecera ya existe justo
    # antes, no se duplica.
    out = []
    for l in diff_lines:
        if l.startswith("--- "):
            if not out or not out[-1].startswith("diff --git "):
                old = l[4:].strip()
                path = old[2:] if old.startswith("a/") else old
                out.append(f"diff --git a/{path} b/{path}")
        out.append(l)
    return "\n".join(out) + "\n"


def _code_context(files, keywords=()):
    """Recorta el código relevante (igual que evolution_engine.draft_patch).

    Tope TOTAL MAX_CONTEXT_CHARS (no por archivo). Dos pasadas para que
    ningún archivo grande robe el presupuesto de los demás: primero entran
    íntegros los archivos pequeños; el resto se reparte entre los grandes
    como ventanas centradas en el primer símbolo del hallazgo (p. ej.
    scan_saf_guards en la línea 325 de evolution_engine.py, no las
    primeras 200 líneas inútiles)."""
    parts = []
    budget = MAX_CONTEXT_CHARS
    # Pasada 1: archivos pequeños íntegros.
    big = []
    for f in files or []:
        p = REPO / f
        if not p.exists() or p.is_dir():
            parts.append(f"### {f}\n(no existe en el árbol: se creará)\n")
            continue
        src = p.read_text(errors="ignore")
        if len(src) <= budget:
            parts.append(f"### {f}\n```\n{src}\n```\n")
            budget -= len(src)
        else:
            big.append((f, src))
    # Pasada 2: los grandes se reparten el resto a partes iguales.
    if big:
        share = max(1200, budget // len(big))
        for f, src in big:
            if budget <= 0:
                parts.append(f"### {f}\n(omitido: presupuesto de contexto del consejo agotado)\n")
                continue
            window = _relevant_window(src, keywords, min(share, budget))
            parts.append(f"### {f}\n```\n{window}\n```\n")
            budget -= len(window)
    return "".join(parts)


_DETECTOR_SYSTEM = (
    "Eres el DETECTOR DE CÓDIGO REAL del consejo multi-modelo de IVANNA. "
    "Tu trabajo es verificar en el CÓDIGO FUENTE REAL si un hallazgo es un "
    "problema REAL o un FALSO POSITIVO, y entregar el mapa de archivos y "
    "símbolos afectados. Lee el código que se te dio: ¿el defecto existe de "
    "verdad? ¿La guarda / el símbolo / el test que el hallazgo dice que "
    "falta existe en otro archivo, en un header incluido, o con otro nombre "
    "equivalente? NO opines de diseño: eso lo hace el ARQUITECTO. Abre tu "
    "respuesta con una sola palabra: REAL o FALSO POSITIVO. Después, el "
    "motivo concreto en 2-3 frases, nombrando el archivo y la línea o "
    "función que lo demuestra, y la lista de archivos/símbolos afectados. "
    "Sé preciso: no inventes APIs ni líneas que no estén en el código dado.")

_VERIFIER_SYSTEM = (
    "Eres el VERIFICADOR EMPÍRICO del consejo multi-modelo de IVANNA. Tu "
    "trabajo es validar que cada número, frecuencia, umbral, ciclo de disco "
    "o ganancia del hallazgo provenga de una MEDICIÓN REAL (teléfono, logs "
    "o código) y no de una suposición, un default de código o un umbral de "
    "test. Revisa la evidencia: cada cifra debe tener fuente (archivo, "
    "línea, log o medición). Abre con una sola palabra: VERIFICADO o NO "
    "VERIFICADO. Después, la lista de cifras con su fuente, y las que no "
    "tienen fuente marcadas como SOSPECHOSAS. No inventes mediciones.")

_PROPOSER_SYSTEM = (
    "Eres el ARQUITECTO JEFE Y PRESCRIPTOR del consejo multi-modelo de "
    "IVANNA (C++, Kotlin, Python). Eres el cerebro del consejo: recibes el "
    "hallazgo verificado por el DETECTOR y las métricas del VERIFICADOR y "
    "redactas la PRESCRIPCIÓN QUIRÚRGICA EXACTA — archivo, método, lógica "
    "y borde de rollback — que el IMPLEMENTER convertirá en parche. En "
    "cambios pequeños o críticos (menos de 120 líneas), genera tú mismo el "
    "bloque de código de referencia. El CENTINELA revisará regresiones; el "
    "ÁRBITRO dará el veredicto final. Estándar REFINADA MAGISTRAL y REGLA "
    "DE ORO: la solución debe elevar IVANNA POR ENCIMA de lo mejor que "
    "existe. Un arreglo que solo deja el producto 'bien' o 'a nivel' NO es "
    "suficiente. Fail-closed: ante entradas no válidas, el código nuevo "
    "degrada a un estado seguro sin tocar el estado. Devuelve SOLO la "
    "prescripción como prosa estructurada: ENFOQUE (qué cambiar y por qué, "
    "la idea nueva, en 2-3 frases); ARCHIVOS (qué archivos/funciones toca y "
    "por qué esos); PASOS (los pasos concretos, con el código de referencia "
    "si el cambio es pequeño); TEST (qué test de regresión demuestra que el "
    "defecto quedó corregido); RIESGOS (qué podría degradarse y cómo se "
    "evita). Sé concreto: nombra archivos y funciones reales del código que "
    "se te dio. No inventes APIs, métricas ni resultados de tests.")

_IMPLEMENTER_SYSTEM = (
    "Eres el IMPLEMENTER ENJAULADO del consejo multi-modelo de IVANNA "
    "(C++, Kotlin, Python). Tu trabajo es traducir la prescripción del "
    "ARQUITECTO a un unified diff que compila, bajo REGLA DE MUNDO CERRADO: "
    "usa SOLO los símbolos de la tabla ALLOWED_SYMBOLS que se te dio y SOLO "
    "las líneas EXACTAS del código dado (copia el contexto tal cual, sin "
    "inventar ni una línea). Si falta un dato o un símbolo no está en la "
    "tabla, responde MISSING_SYMBOL en vez de inventarlo. No toques archivos "
    "fuera de los indicados. No inventes APIs, métricas ni resultados de "
    "tests. Añade el test de regresión que pide la prescripción cuando el "
    "cambio toca código con tests. Estándar REFINADA MAGISTRAL: reescritura "
    "completa y refinada, nunca un parche mínimo a medias. Fail-closed: ante "
    "entradas no válidas, el código nuevo degrada a un estado seguro sin "
    "tocar el estado. Devuelve SOLO un bloque ```diff con el unified diff "
    "completo (rutas a/<ruta> y b/<ruta>, hunks con números de línea "
    "@@ -a,b +c,d @@), sin prosa fuera del bloque.")

_REVIEWER_SYSTEM = (
    "Eres el CENTINELA ANTI-REGRESIÓN del consejo multi-modelo de IVANNA. "
    "Tu único trabajo es revisar el parche del IMPLEMENTER buscando "
    "REGRESIONES: race conditions, null pointers, locks de audio, ciclo de "
    "vida Android y consumo. PROHIBIDO vetar una mejora como 'falso "
    "positivo' — la realidad del hallazgo ya la decidieron el DETECTOR y el "
    "VERIFICADOR — y PROHIBIDO pedirte que escribas diffs. El parche que "
    "revisas ya aplica limpio y compila (pasó el gate mecánico). Responde "
    "con una checklist breve: cada riesgo con archivo y línea, o 'SIN "
    "REGRESIONES' si no hay ninguno. Abre tu respuesta con una sola palabra: "
    "APROBADO o RECHAZADO. Si rechazas, los arreglos exactos que exiges.")

# REVIEWER2 (0045): gemini-3-flash-preview ocupa el asiento (CRITIQUE 1.0
# + DETECT 1.0 medidos; responde 2.4-12.8 s). La cuarentena era por el
# timeout 107 s de 0035, que NO se reproduce con ping mínimo ni con el
# bench. gemini-3.8-flash sigue en cuarentena (null x3 en 0035; timeout
# 273 s en 0045).
_REVIEWER2_SYSTEM = (
    "Eres EL CONTRAPESO INDEPENDIENTE, el segundo revisor del consejo "
    "multi-modelo de IVANNA. Eres de otra familia de modelos que el primer "
    "revisor (Gemini frente a GPT): tu trabajo es dar la segunda opinión "
    "independiente sobre el parche del IMPLEMENTER, sin dejarte influir por "
    "que el primer revisor ya lo haya aprobado. Revisa: (1) ¿corrige la "
    "causa raíz o solo el síntoma? (2) ¿degrada algo (rendimiento, sonido, "
    "seguridad, tests)? Cualquier degradación es motivo de RECHAZO. "
    "(3) ¿es magistral y completo, o un parche a medias? Un parche que solo "
    "deja el producto 'bien' o 'a nivel' NO es magistral: la regla de oro "
    "exige MEJOR QUE LA ÉLITE. (4) ¿el diff aplica limpio, sin inventar APIs "
    "ni romper nada? (5) ¿trae el test de regresión que demuestra el arreglo? "
    "Si el problema es el ENFOQUE (la estrategia es incorrecta, no la "
    "implementación), abre tu rechazo con la palabra ENFOQUE: para que el "
    "PROPOSER reinvente la solución. Si el problema es la implementación, "
    "rechaza con los arreglos exactos que exiges al IMPLEMENTER. Abre tu "
    "respuesta con una sola palabra: APROBADO o RECHAZADO. Después, los "
    "motivos concretos y, si rechazas, los arreglos exactos que exiges. Si el "
    "parche es bueno, apruébalo sin rodeos.")

_ARBITER_SYSTEM = (
    "Eres el ÁRBITRO SUPREMO del consejo multi-modelo de IVANNA. El "
    "CENTINELA revisó el parche del IMPLEMENTER (que ya aplica y compila) "
    "buscando regresiones. Tu veredicto es FINAL y fail-closed, y es un "
    "DOBLE VEREDICTO: separa SUSTANCIA de EJECUCIÓN. Abre tu respuesta con "
    "dos líneas: 'SUSTANCIA: APROBADA' o 'SUSTANCIA: RECHAZADA', y "
    "'EJECUCIÓN: APROBADA' o 'EJECUCIÓN: RECHAZADA'. La SUSTANCIA es el "
    "diseño: ¿la prescripción corrige la causa raíz y eleva IVANNA hacia la "
    "perfección magistral — MEJOR QUE LA ÉLITE — sin degradar nada? La "
    "EJECUCIÓN es el diff: ¿aplica limpio, compila, trae su test y no "
    "degrada nada? Si la sustancia se aprueba pero la ejecución falla, el "
    "veredicto es RECHAZO PARCIAL: el frente NO muere, el IMPLEMENTER rehace "
    "el diff sobre el plano ya aprobado. Si hay la menor duda de degradación "
    "o de arreglo a medias, RECHAZA. Después de las dos líneas, el motivo "
    "en una frase.")


def _gate_check(patch: str, repo=None):
    """Gate mecánico v2.0 (2026-11-17): ¿el diff aplica limpio sobre el
    árbol real? `git apply --check` NO modifica nada (solo verifica).
    Devuelve (aplica: bool, error: str). Sin este gate, 0035 y 0036
    cerraron con diffs que inventaban APIs y no aplicaban. `repo` es el
    árbol contra el que se valida (por defecto el repo real; los tests
    pasan un repo temporal)."""
    import subprocess
    try:
        r = subprocess.run(["git", "apply", "--check", "-"],
                           input=patch.encode(), cwd=repo or REPO,
                           capture_output=True, timeout=30)
        return r.returncode == 0, r.stderr.decode(errors="ignore")[:500]
    except Exception as e:  # noqa: BLE001
        return False, str(e)[:500]


def council_review(finding: dict, llm=None, repo=None) -> dict:
    """El consejo dialoga sobre un hallazgo y devuelve su veredicto.

    Flujo v2.0 — Consejo Nuclear de 6 + Caballería Pro (reorganización
    2026-11-17 13:34 CST; cada rol lo ocupa el modelo más fuerte PARA ESE
    ROL, medido en 0035/0036, no por nombre):
      Fase 0 (local, fuera de esta función): TIERRA FIRME — el llamador
        extrae archivos exactos, fragmentos completos, tabla
        ALLOWED_SYMBOLS, evidencia y etiqueta [MEJORA/DSP]/[BUG]/
        [ARQUITECTURA]. Sin Tierra Firme no se convoca al consejo.
      0. DETECTOR (mistral) verifica el hallazgo en código real: ¿REAL o
         FALSO POSITIVO? Su veredicto se antepone al brief.
      0b. VERIFICADOR (3.1-flash-lite) valida que cada cifra de la
         evidencia sea medición real (no default ni umbral de test).
      1. ARQUITECTO (luna) redacta la PRESCRIPCIÓN QUIRÚRGICA EXACTA (y
         en cambios <120 líneas, el bloque de código de referencia).
      2. IMPLEMENTER ENJAULADO (gpt-oss-120b) redacta el PARCHE bajo
         mundo cerrado ALLOWED_SYMBOLS; el GATE MECÁNICO (git apply
         --check sobre el árbol real) lo valida antes de la revisión: si
         no aplica, rebote directo al implementer (máx. 1 reintento).
      3. CENTINELA (haiku) audita REGRESIONES sobre un parche que ya
         aplica y compila. Si RECHAZA por la implementación, el
         IMPLEMENTER lo revisa; si rechaza por el ENFOQUE (lo marca con
         la palabra ENFOQUE:), el ARQUITECTO reinventa la prescripción en
         la siguiente ronda.
      4. Si el CENTINELA APRUEBA, el ARBITER (mini) da el DOBLE
         VEREDICTO (sustancia vs ejecución).
    Sin consenso en 2 rondas → RECHAZADO (fail-closed). Sustancia
    aprobada + ejecución rechazada → RECHAZO PARCIAL (el frente no muere:
    se rehace el diff sobre el plano aprobado).

    finding: {"kind", "title", "evidence", "files": [rutas relativas]}.
    Devuelve {"approved", "patch", "rounds", "detector", "reviewer",
    "arbiter", "reason"}. Cada ronda registra {"round", "proposer",
    "implementer", "reviewer"}. NUNCA aplica nada al árbol ni valida en
    scratch: eso lo hace el llamador (evolution_engine.validate_proposal)."""
    llm = llm or _llm()
    repo = Path(repo) if repo is not None else REPO
    context = _code_context(finding.get("files") or [],
                            _keywords_from_finding(finding))
    brief = (
        f"HALLAZGO ({finding.get('kind', '?')}): {finding.get('title', '')}\n"
        f"Evidencia medida:\n{finding.get('evidence', '')}\n\n"
        f"CÓDIGO RELEVANTE:\n{context}\n"
        "REESCRITURA EXIGIDA: corrige la causa raíz de forma completa y "
        "refinada (no un parche mínimo), con test de regresión si el cambio "
        "toca código con tests. Si el hallazgo es solo de detección (falso "
        "positivo), corrige el detector y añade su test unitario. REGLA DE "
        "ORO: el resultado debe ser MEJOR QUE LA ÉLITE, mejor que la "
        "tecnología existente — no solo 'bien' o 'a nivel'.")
    # 0. DETECTOR verifica el hallazgo (¿problema real o falso positivo?)
    # antes de que el consejo gaste un ciclo en él. Su veredicto se
    # antepone al brief para que el proposer sepa si corrige el producto
    # o el detector. Si el detector no responde (proveedor caído), se
    # registra NO DISPONIBLE y se continúa sin él (el proposer decide con
    # la evidencia del hallazgo); nunca se tumba el consejo por esto.
    detector_verdict = None
    try:
        detector_text = llm.chat(
            [{"role": "system", "content": _DETECTOR_SYSTEM},
             {"role": "user", "content": brief}],
            model=ROLES["detector"], max_tokens=1024,
            read_timeout_s=REVIEW_READ_TIMEOUT_S)
        _det_is_real = detector_text.strip().upper().startswith("REAL")
        _det_is_fp = "FALSO POSITIVO" in detector_text.strip().upper()
        detector_verdict = {
            "real": _det_is_real if (_det_is_real or _det_is_fp) else None,
            "text": detector_text[:2000]}
        brief = (f"VEREDICTO DEL DETECTOR ({ROLES['detector']}): "
                 f"{detector_text.strip()[:1500]}\n\n" + brief)
    except Exception as e:  # noqa: BLE001 - el proveedor es inestable
        detector_verdict = {"real": None,
                            "text": "detector NO DISPONIBLE: "
                                    f"{str(e)[:200]}"}
    # 0b. VERIFICADOR valida que las cifras de la evidencia sean
    # mediciones reales (no defaults de código ni umbrales de test). Si
    # el proveedor falla, se registra NO DISPONIBLE y se continúa (el
    # coordinador verifica por grep; su palabra prevalece sobre el
    # verificador — lección del seguimiento 0035).
    verifier_verdict = None
    try:
        verifier_text = llm.chat(
            [{"role": "system", "content": _VERIFIER_SYSTEM},
             {"role": "user", "content": brief}],
            model=ROLES["verifier"], max_tokens=1024,
            read_timeout_s=REVIEW_READ_TIMEOUT_S)
        verifier_verdict = {"text": verifier_text[:2000]}
        brief = (f"VEREDICTO DEL VERIFICADOR ({ROLES['verifier']}): "
                 f"{verifier_text.strip()[:1500]}\n\n" + brief)
    except Exception as e:  # noqa: BLE001 - el proveedor es inestable
        verifier_verdict = {"text": "verificador NO DISPONIBLE: "
                                    f"{str(e)[:200]}"}
    rounds = []
    patch = None
    reviewer_verdict = None
    solution = None
    feedback = ""            # motivos del reviewer para el implementer
    approach_feedback = ""   # motivos del reviewer para el proposer
    need_new_approach = True  # la ronda 1 siempre inventa la solución
    for rnd in range(1, MAX_ROUNDS + 1):
        # 1. PROPOSER inventa la solución (ronda 1, o cuando el reviewer
        #    marcó que el ENFOQUE es incorrecto).
        if need_new_approach:
            proposer_user = brief + (
                "\n\nINVENTA LA SOLUCIÓN (enfoque, no código final): "
                "propón el enfoque nuevo que resuelve la causa raíz y "
                "eleva el producto por encima de la élite.")
            if approach_feedback:
                proposer_user += (
                    "\n\nEL ENFOQUE ANTERIOR FUE RECHAZADO POR EL REVIEWER: "
                    f"{approach_feedback}\nInventa un enfoque DISTINTO, no "
                    "una variante del mismo.")
            solution = llm.chat(
                [{"role": "system", "content": _PROPOSER_SYSTEM},
                 {"role": "user", "content": proposer_user}],
                model=ROLES["proposer"], max_tokens=PROPOSER_MAX_TOKENS,
                read_timeout_s=REVIEW_READ_TIMEOUT_S)
            proposer_note = ("solución inventada" if rnd == 1
                             else "enfoque reinventado")
            need_new_approach = False
        else:
            proposer_note = "sin cambios de enfoque"
        # Tope DINÁMICO de la solución (cada ronda, porque el feedback del
        # reviewer crece y empuja la llamada del implementer por encima del
        # límite de entrada del proveedor ~16k): se recorta la solución a
        # lo que quepa tras restar system, brief y feedback. La cola que se
        # pierde es RIESGOS, lo menos crítico para implementar.
        _room = (MAX_INPUT_CHARS - len(_IMPLEMENTER_SYSTEM) - len(brief)
                 - len(feedback or "") - 400)
        _sol = solution or ""
        if len(_sol) > _room:
            _sol = (_sol[:max(0, _room)]
                    + "\n[... solución recortada: resto omitido por "
                      "límite de entrada del proveedor ...]")
        # 2. IMPLEMENTER redacta el parche a partir de la solución.
        impl_user = brief + f"\n\nSOLUCIÓN DEL PROPOSER:\n{_sol}\n"
        if feedback:
            impl_user += (
                f"\n\nREVISIÓN DEL REVIEWER (ronda {rnd - 1}) — el parche "
                "anterior fue RECHAZADO por estos motivos; revísalo con "
                f"TODOS ellos, sin dejar ninguno sin atender:\n{feedback}")
        # 2. IMPLEMENTER redacta el parche a partir de la solución.
        # Reintentos (2026-11-16, prueba de fuego): el proveedor ddgw
        # devuelve un diff VACÍO de 21 chars ('```diff\n*** End Patch')
        # en ~1 s, de forma intermitente y en VARIOS modelos (medido en
        # haiku y en gpt-5.6-luna: el mismo prompt da 21 chars o un diff
        # completo de 7-8k según el intento). Es un fast-fail del
        # proveedor, no del prompt ni del modelo. Se reintenta hasta 5
        # veces con backoff (5/10/20/40 s); si las 5 fallan, la ronda se
        # da por fallida (fail-closed).
        text, patch_try = None, None
        for _attempt in range(1, 6):
            # Failover 0045: el titular falla CODE medido (gpt-oss-120b
            # 0/2 diffs aplicables, cuenta mal los hunks); tras 2 intentos
            # fallidos entra el respaldo (gemma4-31b, CODE 2/2 medido) con
            # el mismo sistema enjaulado y el mismo gate mecánico.
            _impl_model = (ROLES["implementer"] if _attempt <= 2
                           else ROLES.get("implementer_backup")
                           or ROLES["implementer"])
            text, patch_try = llm.draft_rewrite(
                _IMPLEMENTER_SYSTEM, impl_user, model=_impl_model,
                max_tokens=IMPLEMENTER_MAX_TOKENS, base_dir=REPO)
            if patch_try is None:
                # Plan C: el implementer devolvió texto con un diff plano
                # (--- a/) que extract_patch rechaza. Se intenta rescatarlo.
                patch_try = _extract_plain_diff(text)
            if patch_try is not None:
                break
            if _attempt < 5:
                import time as _time
                _time.sleep((5, 10, 20, 40)[_attempt - 1])
        if patch_try is None:
            rounds.append({"round": rnd, "proposer": proposer_note,
                           "implementer": "sin diff utilizable (5 intentos)",
                           "reviewer": None})
            return {"approved": False, "patch": None, "rounds": rounds,
                    "detector": detector_verdict,
                    "reviewer": None, "arbiter": None,
                    "reason": f"ronda {rnd}: el implementer no devolvió "
                              "un diff utilizable en 5 intentos"}
        patch = patch_try
        # GATE MECÁNICO (v2.0, Fase 2): el parche debe aplicar limpio sobre
        # el árbol real ANTES de llegar al centinela y al arbiter. Si
        # `git apply --check` falla, rebote directo al implementer (máx. 1
        # reintento por ronda) con el error de git; si el reintento tampoco
        # aplica, la ronda se da por fallida (fail-closed) sin gastar al
        # centinela ni al arbiter.
        _gate_ok, _gate_err = _gate_check(patch, repo)
        if not _gate_ok:
            # Reintento del gate con el RESPALDO (0045): el titular ya
            # falló el gate una vez; gemma4-31b cuenta bien los hunks
            # (CODE 2/2 medido) y merece el intento.
            _retry_model = (ROLES.get("implementer_backup")
                            or ROLES["implementer"])
            _retry_text, _retry_patch = llm.draft_rewrite(
                _IMPLEMENTER_SYSTEM,
                impl_user + "\n\nEL PARCHE ANTERIOR NO APLICA (gate "
                f"mecánico, git apply --check):\n{_gate_err}\nRedacta el "
                "diff de nuevo, copiando el contexto EXACTO del código dado.",
                model=_retry_model,
                max_tokens=IMPLEMENTER_MAX_TOKENS, base_dir=REPO)
            if _retry_patch is None:
                _retry_patch = _extract_plain_diff(_retry_text)
            _retry_ok, _ = (_gate_check(_retry_patch, repo)
                            if _retry_patch is not None else (False, ""))
            if _retry_patch is not None and _retry_ok:
                patch = _retry_patch
            else:
                rounds.append({"round": rnd, "proposer": proposer_note,
                               "implementer": "diff no aplicable (gate "
                                              "mecánico)",
                               "reviewer": None})
                return {"approved": False, "patch": None, "rounds": rounds,
                        "detector": detector_verdict,
                        "verifier": verifier_verdict,
                        "reviewer": None, "arbiter": None,
                        "reason": f"ronda {rnd}: el parche no aplica "
                                  "limpio (git apply --check) ni tras 1 "
                                  "reintento"}
        # 3. CENTINELA (antes REVIEWER) audita regresiones del parche.
        # Resiliencia (2026-11-16): si la llamada al reviewer FALLA
        # (excepción del proveedor), se trata como RECHAZO (fail-closed)
        # y se pasa a la siguiente ronda, no se tumba el consejo.
        try:
            review_text = llm.chat(
                [{"role": "system", "content": _REVIEWER_SYSTEM},
                 {"role": "user", "content":
                  f"HALLAZGO: {finding.get('title', '')}\n"
                  f"Evidencia: {finding.get('evidence', '')}\n\n"
                  f"PARCHE DEL IMPLEMENTER (ronda {rnd}):\n```diff\n{patch}\n```\n\n"
                  "Critícalo. Abre con APROBADO o RECHAZADO. Si el problema "
                  "es el ENFOQUE, márcalo con ENFOQUE: al inicio."}],
                model=ROLES["reviewer"], max_tokens=4096,
                read_timeout_s=REVIEW_READ_TIMEOUT_S)
        except Exception as e:  # noqa: BLE001 - el proveedor es inestable
            review_text = ("RECHAZADO\nEl reviewer no pudo ser consultado "
                           f"(error del proveedor: {str(e)[:200]}).")
        approved, reasons = _parse_verdict(review_text)
        reviewer_verdict = {"round": rnd, "approved": approved,
                            "text": review_text[:4000]}
        # 3b. REVIEWER2 (Gemini) critica el mismo parche. El parche
        # necesita que AMBOS revisores aprueben (fail-closed). Si el
        # primero ya rechazó, Gemini no se consulta (ahorra una llamada
        # lenta); si aprobó, Gemini es el segundo filtro.
        # Resiliencia (2026-11-16): Gemini entra en cooldown 429 / 503
        # por demanda con frecuencia (medido: "All credentials cooling
        # down", "high demand"). Si la llamada FALLA (excepción), NO se
        # tumba todo el consejo: se registra reviewer2 como NO DISPONIBLE
        # y se continúa solo con reviewer1 + arbiter (que siguen siendo
        # dos puertas independientes + la validación en scratch). El
        # veredicto deja constancia explícita de la ausencia, para que
        # nadie crea que Gemini aprobó. Si Gemini RESPONDE y rechaza, su
        # rechazo sí bloquea (fail-closed).
        gemini_verdict = None
        if approved is True and ROLES.get("reviewer2"):
            try:
                gemini_text = llm.chat(
                    [{"role": "system", "content": _REVIEWER2_SYSTEM},
                     {"role": "user", "content":
                      f"HALLAZGO: {finding.get('title', '')}\n"
                      f"Evidencia: {finding.get('evidence', '')}\n\n"
                      f"PARCHE DEL IMPLEMENTER (ronda {rnd}):\n```diff\n{patch}\n```\n\n"
                      "Critícalo. Abre con APROBADO o RECHAZADO. Si el problema "
                      "es el ENFOQUE, márcalo con ENFOQUE: al inicio."}],
                    model=ROLES["reviewer2"], max_tokens=4096,
                    read_timeout_s=REVIEW_READ_TIMEOUT_S)
            except Exception as e:  # noqa: BLE001 - el proveedor es inestable
                gemini_verdict = {"round": rnd, "approved": None,
                                  "text": "reviewer2 NO DISPONIBLE: "
                                          f"{str(e)[:200]}"}
            else:
                g_approved, g_reasons = _parse_verdict(gemini_text)
                gemini_verdict = {"round": rnd, "approved": g_approved,
                                  "text": gemini_text[:4000]}
                if g_approved is not True:
                    approved, reasons = g_approved, g_reasons
                    reviewer_verdict = gemini_verdict
        rounds.append({"round": rnd, "proposer": proposer_note,
                       "implementer": "diff redactado",
                       "reviewer": reviewer_verdict,
                       "reviewer2": gemini_verdict})
        if approved is True:
            break
        # RECHAZADO (o ambiguo = fail-closed): ¿quién revisa?
        # ENFOQUE → el proposer reinventa la solución; si no, el
        # implementer revisa el parche con los motivos.
        feedback = reasons
        if "ENFOQUE" in review_text.upper():
            need_new_approach = True
            approach_feedback = reasons
        else:
            need_new_approach = False
        if rnd == MAX_ROUNDS:
            return {"approved": False, "patch": None, "rounds": rounds,
                    "detector": detector_verdict,
                    "reviewer": reviewer_verdict, "arbiter": None,
                    "reason": f"sin consenso en {MAX_ROUNDS} rondas; último "
                              f"rechazo: {reasons[:300]}"}
    # 4. El REVIEWER aprobó: el ARBITER da el veredicto final.
    # Resiliencia (2026-11-16): si la llamada al arbiter FALLA, se trata
    # como RECHAZO (fail-closed: sin veredicto del arbiter no hay parche).
    try:
        arbiter_text = llm.chat(
            [{"role": "system", "content": _ARBITER_SYSTEM},
             {"role": "user", "content":
              f"HALLAZGO: {finding.get('title', '')}\n"
              "El REVIEWER aprobó este parche del IMPLEMENTER. Tu veredicto "
              f"es final:\n```diff\n{patch}\n```\n\n"
              "¿Evoluciona hacia la perfección magistral — MEJOR QUE LA "
              "ÉLITE — y NUNCA degrada? Abre con APROBADO o RECHAZADO."}],
            model=ROLES["arbiter"], max_tokens=1024,
            read_timeout_s=REVIEW_READ_TIMEOUT_S)
    except Exception as e:  # noqa: BLE001 - el proveedor es inestable
        arbiter_text = ("RECHAZADO\nEl arbiter no pudo ser consultado "
                        f"(error del proveedor: {str(e)[:200]}).")
    # DOBLE VEREDICTO (v2.0): el arbiter separa SUSTANCIA de EJECUCIÓN.
    # Si la sustancia se aprueba pero la ejecución falla, el frente NO
    # muere en RECHAZADO: es RECHAZO PARCIAL y el implementer rehace el
    # diff sobre el plano ya aprobado (el llamador lo reintenta con la
    # prescripción guardada en "approved_design").
    _up = arbiter_text.upper()
    _sub_ok = "SUSTANCIA: APROBADA" in _up
    _exe_ok = "EJECUCIÓN: APROBADA" in _up
    arb_approved, arb_reasons = _parse_verdict(arbiter_text)
    # Compatibilidad: si el arbiter responde en formato viejo (una sola
    # palabra APROBADO sin líneas SUSTANCIA/EJECUCIÓN), vale como ambas.
    if arb_approved is True and "SUSTANCIA:" not in _up \
            and "EJECUCIÓN:" not in _up:
        _sub_ok = _exe_ok = True
    arbiter_verdict = {"approved": arb_approved,
                       "substance_approved": _sub_ok,
                       "execution_approved": _exe_ok,
                       "text": arbiter_text[:2000]}
    if _sub_ok and _exe_ok:
        return {"approved": True, "patch": patch, "rounds": rounds,
                "detector": detector_verdict, "verifier": verifier_verdict,
                "reviewer": reviewer_verdict, "arbiter": arbiter_verdict,
                "reason": "consejo de acuerdo: detector + verificador + "
                          "arquitecto + implementer + centinela + arbiter"}
    if _sub_ok and not _exe_ok:
        return {"approved": False, "patch": None, "rounds": rounds,
                "detector": detector_verdict, "verifier": verifier_verdict,
                "reviewer": reviewer_verdict, "arbiter": arbiter_verdict,
                "partial": True, "approved_design": solution,
                "reason": "RECHAZO PARCIAL: sustancia aprobada, ejecución "
                          "rechazada — rehacer el diff sobre el plano "
                          "aprobado, sin reabrir el diseño: "
                          f"{arb_reasons[:300]}"}
    return {"approved": False, "patch": None, "rounds": rounds,
            "detector": detector_verdict, "verifier": verifier_verdict,
            "reviewer": reviewer_verdict, "arbiter": arbiter_verdict,
            "reason": f"el arbiter rechazó: {arb_reasons[:300]}"}


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--finding", required=True,
                    help="hallazgo JSON: {kind, title, evidence, files[]}")
    ap.add_argument("--out", help="archivo JSON donde escribir el veredicto")
    args = ap.parse_args()
    finding = json.loads(args.finding)
    verdict = council_review(finding)
    out = {"finding": finding.get("title", ""), **verdict}
    text = json.dumps(out, indent=2, ensure_ascii=False)
    if args.out:
        Path(args.out).write_text(text)
        print(f"veredicto del consejo: {args.out}")
    print(text)


if __name__ == "__main__":
    main()
