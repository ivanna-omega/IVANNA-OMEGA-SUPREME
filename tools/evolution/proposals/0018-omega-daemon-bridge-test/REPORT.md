# Propuesta 0018 — omega-daemon-bridge-test (Plan Omnipotencia)

**Hallazgo:** `omega_daemon_bridge.cpp` (156 líneas) estaba entre los 19
módulos sin tests: la app recibe el fd del SHM del daemon por SCM_RIGHTS
en `@omega_daemon_socket`, valida el layout y lo mapea. Sin cobertura,
un cambio en el handshake no se detectaba hasta el teléfono.

## Diagnóstico (verificado, no inferido)

- La API es una sola función: `omega_daemon_get_shared_state()`. Sin
  daemon → nullptr (connect falla de inmediato, sin colgarse). Con
  daemon → fd por SCM_RIGHTS, fstat (≥ SHM_SIZE), mmap PROT_READ,
  `validateShmHeader` (magic/version/state_size) y devuelve el estado
  en SHM_STATE_OFFSET. La 2ª llamada devuelve el caché (sin segundo
  handshake).
- El protocolo que espera `receiveExactlyOneFd` (inline en
  omega_control_bus.h): exactamente 1 byte + exactamente 1 fd.
- `OmegaShmManager` crea el backing válido (header + estado).

## El fix

`test_omega_daemon_bridge.cpp` (3 tests, mock daemon en hilo):
1. **NoDaemon:** sin daemon, devuelve nullptr.
2. **Handshake:** el mock daemon escucha en el socket abstracto y
   envía 1 byte + el fd de un SHM válido; el bridge devuelve un estado
   no nulo cuyo header pasa `validateShmHeader`.
3. **Caché:** la 2ª llamada devuelve EL MISMO puntero. El test es
   autocontenido (su propio mock daemon): CTest ejecuta cada TEST en
   un proceso aparte, así que no puede depender del mapeo del test
   anterior. El mock tiene timeout en accept (2 s): si el bridge ya
   tiene el estado cacheado (corrida directa), el mock sale solo en
   vez de colgar el join.

Cableado en `tests/CMakeLists.txt` (+ shm_manager.cpp). Conteo honesto
1052 → 1055 en README + telemetría.

## Validación

- Directo (3 en un proceso): **3/3 PASSED**.
- Vía CTest (cada TEST en proceso aparte): **3/3 PASSED**.
- Suite CTest completa `-j4`: **100% tests passed, 0 failed out of 1055**
  (57.65 s).
- `check_docs_claims`: [PASS] 0 hallazgos.
- Scan módulos sin tests: **16 → 15**.
- Parche validado en scratch: 0013+0015+0016+0017+0018 aplican limpio
  sobre HEAD; 3/3 tests OK.
- Apply dry-run: exact.

## Dependencia

**0018 requiere 0013, 0015, 0016 y 0017 aplicados primero** (todos
tocan `tests/CMakeLists.txt` + conteos).

## Archivos

- `app/src/main/cpp/tests/test_omega_daemon_bridge.cpp` — 3 tests nuevos.
- `app/src/main/cpp/tests/CMakeLists.txt` — cableado.
- `telemetry/ctest_summary.json` — 1055/1055.
- `README.md` — 1055/1055.

**Autoría:** coordinador del Plan Omnipotencia, vía directa
(2026-11-16). Sin consejo.
