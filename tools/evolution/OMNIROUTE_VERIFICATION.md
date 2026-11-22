# Verificación real de OmniRoute y del consejo (2026-11-14/15)

Evidencia MEDIDA, no afirmada. Cada número lleva su fecha y su comando;
si el gateway cambia, estos números caducan y hay que re-medirlos.
"no cobrado en esta prueba" NO equivale a "gratis para siempre".

## Gateway OmniRoute (VM, localhost:20128)

| Fecha (CST) | Prueba | Resultado |
|---|---|---|
| 2026-11-14 ~19:28 | `curl http://localhost:20128/v1/models` sin key | HTTP 401 `{"error":{"message":"Authentication required","type":"invalid_api_key"}}` — vivo, exige key |
| 2026-11-14 ~19:28 | mismo con la key de `~/.omniroute/ivanna-app-key` | HTTP 200, 448 modelos listados |
| 2026-11-14 ~19:30 | `llm_client.health()` | ok=True, latencia 1087 ms, 453 modelos |
| 2026-11-15 ~01:40 | `evolution_engine.py --status` (health) | ok=True, latencia 10,5 ms, 466 modelos |

El nº de modelos CAMBIA entre probes (448 → 453 → 466): el catálogo
del gateway es dinámico. Nunca se hardcodea el total; lo que se afirma
es la presencia de los 6 del combo, medida en cada probe.

## Combo `ivanna-free`: los 6 modelos presentes (medido 2026-11-15 ~01:40)

- ddgw/claude-haiku-4-5 — presente
- ddgw/gpt-5.6-luna — presente
- ddgw/gpt-5.4-mini — presente
- ddgw/mistral-small-2603 — presente
- ddgw/tinfoil/gpt-oss-120b — presente
- ddgw/tinfoil/gemma4-31b — presente

## Llamadas REALES por modelo de rol (2026-11-14 ~19:30)

`llm_client.chat` con prompt trivial ("Responde con una sola palabra:
OK", max_tokens=16, temperature=0.0) a cada modelo del consejo:

| Modelo (rol) | Latencia | Respuesta |
|---|---|---|
| ddgw/tinfoil/gpt-oss-120b (PROPOSER) | 1,2 s | "OK" |
| ddgw/claude-haiku-4-5 (REVIEWER) | 1,0 s | "OK" |
| ddgw/gpt-5.6-luna (ARBITER) | 1,0 s | "OK" |

No son mocks: son completions reales del gateway con la key de la VM.
(Latencias de ese momento; el gateway es lento a veces — medido 37 s
para un "OK" trivial el 2026-11-14 — por eso los timeouts son 180/240 s.)

## Consejo real de punta a punta (ensayo 0006-rehearsal-seeded)

- Intento 1 (2026-11-15 ~01:33): consejo APROBADO en 1 ronda, pero
  validación ROJO — el parche del proposer traía un hunk `@@` sin
  números y `git apply` lo rechazó ("patch with only garbage at
  line 4"). El fail-closed funcionó: nada se publicó. Hallazgo real
  del ensayo → fix en `evolution_engine.normalize_patch_hunks`
  (el consejo NO se tocó).
- Intento 2 (2026-11-15 ~01:34, tras el fix): consejo APROBADO en 1
  ronda (latencia del consejo 5,0 s según REPORT.md; 38,7 s el
  `--evolve` completo), validación VERDE en scratch (unittest 74/74
  OK, scan OK, scratch eliminado). Evidencia:
  `tools/evolution/proposals/0006-rehearsal-seeded/` (REPORT.md,
  COUNCIL.md, validation.json, parche). Ensayo controlado con fallo
  sembrado — NO una reparación real.

## Coste: DESCONOCIDO a priori

- Observado: $0 cobrado en todas las pruebas de 2026-11-14/15 (health
  + 3 chats + 2 consejos completos). No hay factura ni cargo visible.
- NO es prueba de gratuidad permanente: el gateway es de un tercero
  (ddgw) y sus condiciones pueden cambiar. El motor nunca asume coste
  cero: si el gateway devuelve 402/429 por pago/límite, el ciclo se
  detiene (fail-closed) y lo reporta.

## Túnel público (localhost.run)

- Última URL conocida `https://1a12c5e8452bf7.app.lhr.life/v1`:
  502 en el probe del DATA-PLAN de la Vista de Dios (2026-11-14
  ~18:50 CST). NO re-probado por mí en este trabajo: si hace falta,
  re-medir con `curl -s -o /dev/null -w "%{http_code}" <url>/v1/models`.
  El túnel rota y muere; la app no depende de él (el gateway local es
  la vía del motor).

## Seguridad de la key

- La key vive SOLO en `~/.omniroute/ivanna-app-key` (modo 600, host).
  Nunca se imprime, nunca se guarda en el repo, nunca sale en errores
  ni en logs (ver `llm_client.py`).
- `llm_client._split_url` rechaza cualquier host fuera de
  localhost/127.0.0.1/::1: un `IVANNA_OMNIROUTE_URL` ajeno no conecta
  (test en `tests/test_llm_client_security.py`).
- La Vista de Dios NUNCA usa la key: su probe de OmniRoute es el 401
  sin key (vivo) o el fallo de conexión (caído).
