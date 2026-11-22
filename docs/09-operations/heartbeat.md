# Latido PUSH — F8 (v2.5.446)

**Dominio:** IES-001 §07 · Operations.
**Código:** `app/src/main/java/com/ivanna/omega/core/LiveDiagnosticConnector.kt`
(`buildHeartbeat()`, `uploadHeartbeatIfConfigured()`, `reportGatewayReachable()`).
**Tests:** `app/src/test/java/com/ivanna/omega/core/DiagnosticHeartbeatTest.kt`
(correr en CI: `./gradlew :app:testDebugUnitTest`).

## Qué es

El latido es la señal periódica **"estoy vivo + salud mínima"** de la app.
No es telemetría: el snapshot de diagnóstico (cada 15 s) lleva el detalle
(métricas DSP, agente, clips, latencia); el latido lleva solo lo necesario
para saber que el proceso sigue vivo y en qué estado mínimo está.

## Destino — el puente Floot existente

El mismo puente de diagnóstico que ya recibe los snapshots; **no se creó
ningún puente ni destino nuevo**:

- **POST** `https://ivanna-diagnostic-bridge.floot.app/_api/v1/diagnostics`
  (mismo endpoint que el snapshot, mismo token Bearer, mismo opt-in).
- Salud del puente: **GET** `https://ivanna-diagnostic-bridge.floot.app/_api/healthz`
  → `200 {"ok":true,"service":"ivanna-diagnostic-bridge","tokenConfigured":false}`
  (verificado por curl el 2026-11-14; el puente estaba vivo y recibiendo
  snapshots — `sequence: 361` en `/recent` en ese momento).

El latido usa la **misma puerta opt-in** que el diagnóstico: solo se envía
si el usuario activó el envío remoto y configuró endpoint HTTPS + token
(≥ 32 caracteres). Sin opt-in no sale nada, igual que el snapshot.

## Formato exacto

```json
{
  "type": "heartbeat",
  "schemaVersion": 1,
  "sequence": 7,
  "timestampMs": 1793000000000,
  "uptimeMs": 123456,
  "appVersion": "2.5.445",
  "health": {
    "gatewayReachable": true,
    "gatewayKnown": true,
    "dspAlive": true,
    "sessionActive": false
  }
}
```

| Campo | Origen | Significado |
|:---|:---|:---|
| `type` | fijo `"heartbeat"` | Distingue el latido del snapshot de diagnóstico. |
| `schemaVersion` | fijo `1` | Misma versión de esquema que el snapshot (el puente exige `1`). |
| `sequence` | contador propio del latido | **Separado** del contador de snapshots: no se entrelazan. |
| `timestampMs` | `System.currentTimeMillis()` | Hora de pared (epoch ms). |
| `uptimeMs` | `SystemClock.elapsedRealtime()` | Tiempo desde el arranque del dispositivo. |
| `appVersion` | `BuildConfig.VERSION_NAME` | Versión de la app que late. |
| `health.gatewayReachable` | `reportGatewayReachable()` | ¿Responde el gateway OmniRoute (GET `/v1/models`)? |
| `health.gatewayKnown` | idem | `false` = aún sin evidencia de alcance (NO significa "gateway caído"). |
| `health.dspAlive` | `IvannaAgentCore.state.health.adaptiveEngineRunning` | ¿Vive el motor DSP adaptativo? |
| `health.sessionActive` | `IvannaAgentCore.state.perception.playbackActive` | ¿Hay sesión de reproducción activa? |

## Cadencia

- **Latido: cada 60 s** (`HEARTBEAT_PERIOD_MS = 60_000`).
- Snapshot de diagnóstico: cada 15 s.
- Carga conjunta: 5 req/min, muy por debajo del límite del puente
  (60 req/min por IP, `diagnostic-bridge/server.mjs`).

## Qué incluye y qué NO incluye

**Incluye:** tipo, versión de esquema, secuencia, timestamps, versión de la
app y 4 booleanos de salud. Nada más.

**NO incluye:** tokens, claves, endpoints, identificadores de hardware,
nombres de apps, audio, datos personales ni comandos. El test
`heartbeatCarriesNoSecrets` fija el conjunto exacto de claves: si alguien
añade un campo, el test falla.

## Fail-closed

- Si el puente no responde (timeout 4 s, HTTP no-2xx, sin red): se registra
  con `Log.w` y se sigue. **No hay reintento en bucle, no hay cola, no se
  bloquea el audio.** El latido corre en `Dispatchers.IO`, nunca en el hilo
  de audio, en su propia corrutina dentro del `scope` del conector (se
  cancela con `shutdown()`, igual que el snapshot).
- Si el puente rechaza el cuerpo (p. ej. `400 invalid_snapshot`), el fallo
  es visible en logcat (`Heartbeat rejected by endpoint (HTTP 400)`) y el
  snapshot de 15 s sigue su curso normal.

## Cómo verificarlo

```bash
# 1. El puente está vivo:
curl -s https://ivanna-diagnostic-bridge.floot.app/_api/healthz

# 2. Últimos envíos recibidos (snapshots + latidos):
curl -s https://ivanna-diagnostic-bridge.floot.app/_api/v1/diagnostics/recent
```

## ⚠️ Hallazgo verificado: el puente hoy NO conserva `type`/`health`

Verificado el 2026-11-14 contra el puente vivo: `GET /recent` devuelve 20
entradas y **todas** tienen exactamente el conjunto saneado
(`id`, `snapshotId`, `sequence`, `uptimeMs`, `observedAt`, `receivedAt`,
`metrics`, `agent`, `analysis`, `analysisModel`, `analyzedAt`) — sin
`type`, sin `health`, sin `schemaVersion`. Es el mismo comportamiento que
`sanitize()` de `diagnostic-bridge/server.mjs` (conserva solo `metrics` y
`agent`).

**Consecuencia hasta que el puente preserve `type`:**

- El latido se acepta (`202`, cumple `schemaVersion: 1` + `sequence` +
  `uptimeMs` finitos) y queda guardado en `/recent`, pero como una entrada
  con `metrics`/`agent` en `null`: **no distinguible de un snapshot roto**
  por su contenido, solo por su `sequence` (contador propio del latido).
- Cada latido **sobrescribe `latest`** hasta el siguiente snapshot (≤ 15 s,
  porque el snapshot llega 4× más a menudo). El endpoint de análisis
  (`POST /v1/diagnostics/analyze`) analiza `latest`: en esa ventana de
  ≤ 15 s podría analizar un latido (métricas en `null`) en vez del último
  snapshot real.
- Los latidos ocupan ~1 de cada 5 huecos de la ventana de `/recent`
  (20 entradas): 4 snapshots + 1 latido por minuto.

**Lo que falta (fuera del dominio de la app, NO re-desplegar el puente
desde aquí):** que el puente Floot preserve el campo `type` (y `health`)
en lo guardado, excluya los `type: "heartbeat"` de `latest`/análisis y los
muestre filtrables en `/recent`. Con eso, la verificación pasa a ser
`curl .../recent` y filtrar `type == "heartbeat"`. Hasta entonces, la
verificación es: entradas con `metrics`/`agent` en `null` cuya `sequence`
pertenece al contador del latido.

## Pendiente (fuera de este cambio)

- **Cablear `reportGatewayReachable()`:** `OmniRouteOrchestrator` es una
  clase instanciada dentro de `IvannaGeminiAgent`, no un singleton, así que
  el conector no puede leer su salud directamente. Falta **una línea** en
  `IvannaGeminiAgent` (fuera del dominio F8): llamar a
  `LiveDiagnosticConnector.reportGatewayReachable(...)` tras cada
  health-check (`GET /v1/models`). Mientras tanto el latido reporta
  `gatewayKnown: false` (sin evidencia), que es el valor honesto.
- **Ejecutar los tests en CI** (`./gradlew :app:testDebugUnitTest`): en
  esta VM no hay Android SDK, así que los tests quedaron escritos sin
  ejecutar. No se declara que pasen.
- **Verificación en vivo con token real:** el POST exige el Bearer del
  puente; no se probó el envío real del latido (no se tocan credenciales
  desde aquí). La primera verificación real es en el teléfono con el
  opt-in activado: logcat `Heartbeat accepted (HTTP 202)` + entrada en
  `/recent`.
