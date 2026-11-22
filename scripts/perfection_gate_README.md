# Puerta de Perfección — `scripts/perfection_gate.py`

**Sección L2 del prompt maestro v3 ("LEY DE PERFECCIÓN"). Ejecutor: `scripts/perfection_gate.py`.**

## Qué es

Un script versionado, ejecutado en CI desde clon limpio, que lee el inventario,
ejecuta los verificadores y las suites de tests, y calcula 4 contadores:

| Contador | Significado |
|---|---|
| `PENDIENTES` | Capacidades en estado PENDIENTE + trabajo documentado como pendiente sin evidencia de cierre. |
| `FALLOS` | Verificadores con exit != 0 + tests que fallan + violaciones RT confirmadas + conteos CTest/SOFA distintos del umbral + inventario sin `target_state`. |
| `A_MEDIAS` | Código sin llamador, envoltorio sin integrar, docs que afirman lo que el código no cumple, capacidad por debajo de su objetivo, TODO/FIXME/XXX estricto, prueba en skip/xfail/disabled, flag apagada que debería estar encendida, CTest no ejecutado. |
| `BLOQUEOS_ABIERTOS` | Capacidades BLOQUEADA sin decisión registrada de Esse. |

**Veredicto binario:** exit 0 SOLO si los cuatro son cero. Nadie escribe los
contadores a mano: el script los calcula siempre. La flota no puede decir
"terminado / perfecto / resuelto" si la puerta no dio 0 sobre el commit
exacto reportado.

## Uso

```bash
python3 scripts/perfection_gate.py                 # corrida completa
python3 scripts/perfection_gate.py --skip-ctest    # sin CTest (rápida)
python3 scripts/perfection_gate.py --output /tmp/reporte.json
```

Salida: `perfection_gate_report.json` (JSON legible por máquina: los 4
contadores + detalle por ítem con archivo/línea/símbolo) y resumen humano
en stdout. El JSON es generado en cada corrida y NO se commitea.

La puerta funciona **sin NDK/SDK**: solo Python 3 estándar + los
verificadores. CTest es opcional: si no hay cmake/ctest, se registra
"no ejecutado en este entorno" (A_MEDIAS) sin inventar un resultado.

## Corrección L4 — el estado objetivo es POR TIPO (aprobada por Esse)

El v3 decía "toda capacidad: verificada en dispositivo como mínimo". Eso es
un **error de categoría** para capacidades que NO corren en el teléfono
(consejo multi-modelo, motor de evolución, puente GitHub, Vista de Dios):
exigirles verificación en dispositivo las dejaría en PENDIENTE para siempre
y la puerta nunca llegaría a cero por una razón falsa.

Corrección aplicada: cada capacidad lleva un campo `target_state` en
`docs/capabilities/inventory.yaml`, según su tipo:

| Tipo | `target_state` |
|---|---|
| Audio/DSP de sonido (criterio con ABX / escucha) | `VERIFICADA EN DISPOSITIVO + VALIDADA PERCEPTUALMENTE` |
| Audio/DSP (resto) | `VERIFICADA EN DISPOSITIVO` |
| Herramientas / infraestructura | `VERIFICADA EN HOST/CI` |

La validación perceptual la hace **solo Esse**. La asignación por capacidad
está en el inventario (11 sonido, 9 audio/DSP resto, 7 herramientas/infra).
Una capacidad con `estado` por debajo de su `target_state` cuenta como
A_MEDIAS; en objetivo, no cuenta nada. Sin `target_state`, la puerta marca
FALLO (el inventario viola el esquema L4). Equivalencia documentada:
`estado` "VERIFICADA EN HOST" satisface `target_state` "VERIFICADA EN
HOST/CI" (mismo lugar de verificación; única equivalencia admitida).

## Umbrales (versionados, ANTES de medir)

En `perfection_gate.py`, `THRESHOLDS_VERSION = perfection-gate-thresholds-v1`:

| Umbral | Valor |
|---|---|
| CTest esperado | **1008/1008** |
| SOFA esperado (repo) | **694 archivos** |

**Cambiar un umbral requiere a Esse.** No se baja un umbral para que la
puerta dé cero: si el conteo real es menor (p. ej. la telemetría actual dice
974/974 frente al umbral 1008/1008), la puerta lo reporta como FALLO y lo
decide Esse, no la puerta. Esa discrepancia es un hallazgo, no un motivo
para reescribir el umbral.

**Discrepancia abierta (2026-11-15, commit `3fdad25e`, por Esse):** la
telemetría se sincronizó a **1019/1019** medidos en suite local completa
(1008 CI v2.5.444 + 11 nuevos: rt_log_ring 4 + bofo_isolation_matrix 7).
El umbral de la puerta sigue en **1008/1008** hasta que Esse decida el
cambio: la puerta no se auto-ajusta a una medición nueva, por documentada
que esté. Cuando CTest corra en CI, la puerta comparará el conteo real
contra 1008/1008 y reportará la diferencia como FALLO hasta entonces.

## Reglas de conteo (exactas)

- **Un verificador con exit != 0 = 1 FALLO** (la corrida). Cada hallazgo
  individual `[FAIL]` que reporta = 1 A_MEDIAS (el ítem concreto), salvo
  violaciones RT, que son FALLO cada una. Un mismo defecto puede aparecer
  en FALLOS y en A_MEDIAS porque miden cosas distintas (corridas vs ítems);
  no es doble conteo encubierto.
- **PENDIENTES:** capacidades PENDIENTE + bullets de `pendientes_resumen`
  sin evidencia de cierre, salvo que ya estén contados como BLOQUEADA
  (cap-009, cap-014, cap-025) o como la propia capacidad PENDIENTE.
- **BLOQUEOS_ABIERTOS:** una capacidad BLOQUEADA solo deja de contar cuando
  su bloque del inventario lleva un campo `decision_esse:` con la decisión
  de Esse que la resuelve. Una decisión mencionada en prosa no basta: debe
  quedar registrada con ese campo para que la puerta la vea.
- **TODO/FIXME/XXX:** solo la marca (con ámbito opcional entre paréntesis)
  seguida de dos puntos al inicio de un comentario. Un grep ingenuo es inválido aquí: "TODO" es palabra española
  ("por defecto TODO apagado") y "XXX:" casa con placeholders como
  `"1:XXXXXXXXXXXX:android:..."`. Por eso el escaneo es estricto y está
  documentado en el script.
- **Skips y TODOs:** se excluye el código vendoreado (`third_party`,
  `vendor`, `nlohmann`, `googletest`): un TODO dentro de `json.hpp` de
  nlohmann no es un pendiente del producto. Los skips se escanean solo en
  directorios de tests excluyendo `third_party` (el googletest vendoreado
  documenta `GTEST_SKIP` y `DISABLED_` en sus propios docs y fuentes;
  contarlos sería un falso positivo masivo).
- **Flags apagadas:** los 4 atómicos del efecto que arrancan en 0.0
  (bypass) mientras la app declara default > 0 (cap-001/002/003/007) y
  NEU·Ω OFF por defecto (cap-005) cuentan como A_MEDIAS cada uno.
- **SOFA:** conteo de `*.sofa` en el repo (694 esperado: 455 en
  `app/src/main/assets` + 239 en el módulo).
- **Orden de ejecución:** inventario → escaneos → SOFA → CTest (y, solo
  con `--regenerate-telemetry`, regeneración de
  `telemetry/ctest_summary.json` desde el log CTest, como hace
  `tests-host.yml`) → verificadores → unittest. Por defecto la puerta NO
  regenera la telemetría: no ensucia el árbol de trabajo. En CI sí se
  regenera, antes de `check_docs_claims.py`, para que el conteo CTest que
  verifica sea el de la corrida actual y no una telemetría vieja.

## Lo que la puerta NO puede verificar sola

- **Parche sin rollback probado:** no hay forma automática de verificarlo.
  Cada parche debe documentar su rollback y probarlo; la puerta lo lista
  en `not_verifiable_automatically`.
- **Controles UI sin efecto medible:** la puerta usa como proxy los
  wrappers sin llamador de `check_kotlin_wrapper_wiring.py`. La medición
  perceptual de cada control la hace solo Esse.
- **Validación perceptual (ABX):** solo Esse puede darla; la puerta solo
  registra su ausencia como PENDIENTE.

## Anti-trampas (L6)

Prohibido, para cualquiera (incluida la flota y quien mantiene la puerta):

- Borrar, omitir o debilitar pruebas.
- Bajar umbrales o reducir el alcance de la puerta.
- Reclasificar un fallo como A_MEDIAS (o viceversa) para que el veredicto
  salga en verde.
- Editar la puerta, los contadores o la política de contención para que
  el resultado sea favorable.
- Editar el inventario para subir un estado sin evidencia citada.

**FALSO POSITIVO:** solo con símbolo + ruta + reproducción, revisado por
un agente distinto del que lo reporta. Sin esos tres elementos, el
hallazgo queda.

**Quién implementa no verifica:** la verificación la hace otro agente,
desde clon limpio, corriendo la puerta sobre el commit exacto.

**Muestreo aleatorio:** los ítems cerrados se re-verifican con un agente
nuevo que no participó en el cierre.

## Auditoría de la puerta (L6)

`tools/evolution/tests/test_perfection_gate.py` siembra un defecto (un
TODO estricto en un archivo temporal + una capacidad marcada PENDIENTE en
un inventario temporal) y verifica que la puerta lo atrapa (contador > 0,
exit != 0). Luego lo retira (directorio temporal, limpieza automática).
Esto prueba que la puerta no es decorativa. También verifica que un
inventario limpio (todo en objetivo, sin TODOs) da 0 y exit 0, y que los
umbrales y los `target_state` del inventario real son válidos.

```bash
python3 -m unittest tools.evolution.tests.test_perfection_gate -v
```

## CI

`.github/workflows/perfection-gate.yml`: checkout limpio → instala g++-14
y cmake → corre la puerta con `--regenerate-telemetry` → publica
`perfection_gate_report.json` como artefacto. El job falla si la puerta
falla (eso es lo correcto: una puerta en rojo es la señal honesta, no un
workflow roto). **No se marca como check requerido** hasta que la puerta
llegue a 0: mientras tanto es veredicto informativo, no bloqueo de merges.

## Directiva ELEVAR, JAMÁS DEGRADAR (Esse, 2026-11-14 20:01 CST)

Todo lo que se haga debe ELEVAR, magistral — jamás degradar. Cada cambio,
cada fix, cada decisión debe dejar el producto mejor de lo que estaba:
nunca peor, nunca a medias, nunca un retroceso. Si algo no eleva, no se hace.

Aplicado a esta puerta: la puerta **eleva** porque añade verificación
rigurosa sin tocar código de producto, sin bajar umbrales, sin eliminar
pruebas y sin cambiar estados sin evidencia. Un veredicto NO PERFECTA con
la lista exacta de lo que falta es una elevación frente a no saberlo: es
el mapa de lo que hay que elevar, no un fracaso. La puerta misma debe ser
magistral — clara, rigurosa, sin trampas — y cualquier cambio futuro a la
puerta se juzga con el mismo listón: si no la hace más rigurosa y más
clara, no se hace.
