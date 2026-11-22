# Propuesta 0007-saf-latent-guard-detector — detector SAF por definición

- **Hallazgo:** el scan marcaba `ivanna_saf_apply_latent`
  (`saf_latent_bridge.cpp`) sin guarda, pero el .cpp SÍ tiene guarda:
  llama a `ivanna::safmus::isFiniteLatent(q)`, definida en
  `spatial/SafmusLatentGuard.hpp:11` con `std::isfinite` en el cuerpo.
  Falso positivo del detector por partida doble: (1) buscaba el literal
  `isfinite` case-sensitive y no casa `isFiniteLatent`; (2) solo revisaba
  headers hermanos, no seguía el `#include`.
- **Consejo:** RECHAZÓ 4 veces (0007 ×4: gpt-oss-120b ×3, mistral ×1).
  El detector (haiku) acertó siempre ("FALSO POSITIVO" con el diagnóstico
  exacto). El reviewer (luna) acertó siempre y exigió el listón correcto:
  extractor robusto de definiciones C++ con brace-matching, semántica por
  definición, resolución transitiva de includes, sin fallback legacy,
  contrato de salida intacto. Ningún implementer disponible lo entregó
  en 3 rondas. La prueba de fuego queda VALIDADA por esta vía directa.
- **Autor del parche:** COORDINADOR (no el consejo). El consejo no pudo
  producir un parche aprobable en 4 ciclos; el coordinador redactó el fix
  directo siguiendo el criterio del reviewer y lo validó.
  `apply_proposal.py --dry-run --allow-no-council-record`:
  `already_applied` exact (el parche es exactamente lo que está en el
  árbol).
- **Parche (2 archivos):**
  - `tools/evolution/evolution_engine.py`: guarda por DEFINICIÓN — una
    guarda es una función cuyo cuerpo contiene `std::isfinite`,
    identificada con el extractor `_extract_function_definitions()`
    (brace-matching; maneja firmas multilínea, inline/static/constexpr,
    noexcept, trailing return, ref-qualifiers, listas de
    inicializadores y llaves anidadas; `_strip_cpp_noise()` quita
    comentarios y literales para no ver llaves falsas). Resolución
    transitiva de `#include` con normalización de paths
    (`_resolve_includes()`). Sin fallback legacy de substring
    `isfinite` (eliminado por completo). El módulo está guardado si una
    guarda está definida en el propio .cpp, en su grafo de includes o
    en un header hermano (regla 3 histórica, ahora por definición).
    Contrato de salida intacto (file, functions, has_isfinite_guard,
    guard_source "none", header_checked).
  - `tools/evolution/tests/test_scan_saf_guards.py` (6 tests nuevos):
    el caso real 0007 (include + llamada), include transitivo a dos
    niveles, negativo (función incluida sin std::isfinite sigue siendo
    hit), guarda en el propio .cpp (firma multilínea + llaves
    anidadas), `std::isfinite` en comentario/cadena NO es guarda,
    extractor (cualificadores, llaves anidadas, sin confundir llamadas
    ni control con definiciones).
- **Decisión de diseño documentada (semántica de presencia):** el módulo
  está guardado si una guarda está DEFINIDA en sus archivos (el .cpp, su
  grafo de includes o un header hermano), no solo si el .cpp la llama.
  Es la semántica del patrón de referencia documentado (saf_runtime.cpp:
  SAFUpdate vive en su header hermano y el módulo se considera guardado)
  y la que exige el test existente. Una semántica por llamada
  (call-graph) es un refinamiento futuro posible, no parte de este fix.
- **Validación:**
  - Nuevos tests: **6/6 OK** (suite del archivo 7/7).
  - Scan real: **0 hits SAF** (antes 1, el falso positivo) sin crear
    nuevos hallazgos.
  - Suite evolución completa: **145/145 OK** (139 + 6 nuevos).
  - `apply_proposal.py --dry-run`: `already_applied` exact.
- **Archivos:** `tools/evolution/evolution_engine.py`,
  `tools/evolution/tests/test_scan_saf_guards.py`
- **Veredicto:** VERDE — lista para revisión/commit (por el operador).
  La prueba de fuego 0007 queda VALIDADA.
