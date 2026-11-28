# Propuesta 0034 — Candidata v1.1 (perfil A/B, ejemplo canónico del `repro:` C12)

- **Candidata:** perfil `omega-1795857152818-9lec-v1.1.ivanna.json`
  (MuseLink omega-1795857152818-9lec, micrófono ERB, 177 sesiones,
  2026-11-28 04:09 CST). **NO APLICADO** — pendiente de validación A/B
  automática: 3 sesiones idénticas vs v1.0, mejora ERB mínima 20% sin
  perder coherencia, rollback automático si empeora.
- **Estado:** CANDIDATA. Su validación es el `repro.json` de este
  directorio, verificado en CI por `tools/evolution/check_repro.py`.
- **Sin `.patch`:** es una candidata de perfil A/B, no un parche de
  código. `evolution_engine.validate_proposal` y `publish_proposal` no
  le aplican (exigen parche); su validación ES el repro (C7+S4, ambos
  ejecutables desde que C7 aterrizó el 2026-11-28).

## Contenido del perfil v1.1

Corrección propuesta (Muse IA, 2026-11-28 04:00, 84 sesiones): subgraves
+1, graves −3, medios-graves −2, medios +4, presencia +9, brillo +9 dB.
Mapeo a los 4 slots del motor v1.0 con clamp ±2 dB: low_60_120 −2.0,
mid_500_1k +2.0, high_2k_4k +2.0, presence_4k_8k +2.0. Neto +4.0 dB,
trim −4.0 dB (ganancia neta 0.0 dB, Regla de Oro Matched-LUFS). Residual
+7 dB en presencia/brillo: no lo puede aplicar el motor v1.0 en un solo
perfil; se cierra por evoluciones sucesivas validadas por A/B o por una
ampliación del esquema. Operadores sin cambios (Orbe 0.53, Holos 0.34,
Neuro 0.35, SalaViva 0.30). Fingerprint ERB verificado: ERB-3 209 Hz
+9.86 dB; error RMS espectral 10.46 dB; coherencia media 0%.

## Repro (C12)

`repro.json` (sidecar JSON junto a este REPORT.md — ver
`tools/evolution/check_repro.py` para la desviación documentada
respecto al bloque `repro:` literal de la propuesta: las propuestas
reales no tienen YAML y `council.py` nunca lee estos directorios):

1. **C7 (ejecutable):**
   `python3 scripts/check_muselink_ab.py --baseline tools/evolution/fixtures/ab/v1.0_sessions.json --candidate tools/evolution/fixtures/ab/v1.1_sessions.json --candidate-profile tools/evolution/fixtures/omega-1795857152818-9lec-v1.1.ivanna.json --seed 1795857152818`
   → exit 0, veredicto `PROMOTED` con las 5 puertas en PASS (mejora ERB
   25.1% — RMS 10.56 → 7.92 dB; coherencia 0.000 → 0.000; sin resonancia
   nueva; sin clip 0/36 chunks; S8 endurecido PASS). Los 6 literales
   están en el `expected` del `repro.json`, extraídos verbatim de la
   corrida real (3 corridas idénticas con la semilla fija).
   **Reconciliación C7 (2026-11-28):** durante la tarea C12 el agente
   C7 aterrizó el script y los fixtures; las rutas reales son
   `tools/evolution/fixtures/ab/*`, no `fixtures/ab/*` como decía el
   encargo documentado, y `--baseline-profile` se omite porque el perfil
   v1.0 no existe en el repo (la ayuda de C7 lo marca opcional: «la
   puerta no lo necesita»). **Caveat (del propio C7):** los fixtures
   `ab/*` son SINTÉTICOS (`synthetic=true`); la exportación ERB
   agregada de las 177 sesiones reales está pendiente del servidor. El
   PROMOTED es reproducible y determinista, pero NO es evidencia de
   sesiones medidas: cuando lleguen los fixtures reales, sus sha256
   sustituyen a estos y el `expected` se re-deriva de la corrida real.
2. **S4 (ejecutable):**
   `python3 scripts/validate_ivanna_profile.py --live-session tools/evolution/fixtures/omega-1795857152818-9lec-v1.1.ivanna.json`
   → exit 0, salida `VÁLIDO (schema v1.0 live-session, desconocido —
   Sesión en vivo IVANNA)`. Fixture con sha256 real
   (`c6a15ecc…a48`).

## Verificación local

```sh
python3 tools/evolution/check_repro.py --proposal 0034-v1.1-ab-candidate
# OK  0034-v1.1-ab-candidate: paso C7: OK (exit 0, expected presente)
# OK  0034-v1.1-ab-candidate: paso S4: OK (exit 0, expected presente)
# check_repro: 1 propuesta(s) con repro válido: 0034-v1.1-ab-candidate
```
