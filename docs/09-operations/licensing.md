# Licencias en la app (v2.5.446)

Validación de licencias de IVANNA OMEGA SUPREME dentro de la app.
Estado: **implementado, PENDIENTE de verificación** — el servidor de
licencias está construido y probado (8/8) pero **SIN desplegar**, y los
tests Kotlin no se han ejecutado (no hay Android SDK en la VM).
Nada de lo aquí descrito debe presentarse como "listo" hasta que el
servidor responda en `https://ivanna-license-server.onrender.com` y
la CI ejecute `testDebugUnitTest` en verde.

## Contrato (fuente de verdad)

Fuente: `monetization/license-server/README.md` y
`monetization/license-server/licenses.mjs` (lógica pura del servidor).

| Elemento | Valor |
|---|---|
| Endpoint | `POST {baseUrl}/v1/licenses/validate` |
| Petición | `{licenseKey, deviceId}` |
| `licenseKey` | Forma canónica `IVANNA-XXXX-XXXX-XXXX` (alfabeto Crockford, sin 0/O, 1/I/L). La app la normaliza igual que el servidor (mayúsculas, con o sin guiones/espacios). |
| `deviceId` | SHA-256 del ANDROID_ID, calculado por la app. El servidor lo vuelve a hashear antes de guardarlo: nunca viaja ni se guarda en claro. |
| Éxito | `200 {valid:true, plan, expiresAt, devicesBound, devicesMax}` |
| No válida | `200 {valid:false, reason}` — `not_found` \| `revoked` \| `expired` \| `device_limit` |
| Petición mala | `400 {valid:false, reason:"bad_request"}` (clave con forma inválida o `deviceId` vacío) |
| Planes | `lifetime` (`expiresAt: null`, sin caducidad) y `monthly` (30 días) |
| Dispositivos | Máx. 3 por defecto; el servidor vincula el dispositivo si hay hueco |

La respuesta de éxito NO incluye `newlyBound` (el servidor lo
retira antes de responder). La clave en claro NUNCA se guarda en el
servidor (solo su hash SHA-256) ni se registra en logs: en la app
solo aparece su huella de 8 hex (`LicenseKeyUtils.fingerprint`),
igual que el `fingerprint` del servidor.

## Archivos

| Archivo | Qué es |
|---|---|
| `app/src/main/java/com/ivanna/omega/licensing/LicenseModels.kt` | Planes, motivos, resultado de validación, `LicenseState` (lo que ve la UI) y la caché |
| `.../licensing/LicenseKeyUtils.kt` | Normalización de la clave, SHA-256 del ANDROID_ID, huella, parseo de `expiresAt` (puro, sin Android) |
| `.../licensing/LicenseClient.kt` | POST a `/v1/licenses/validate` (HttpURLConnection, como Tidal); transporte inyectable para tests |
| `.../licensing/LicenseConfig.kt` | URL por defecto y nombres de preferencias |
| `.../licensing/LicenseStore.kt` | Persistencia (SharedPreferences `ivanna_licensing` + store en memoria para tests) |
| `.../licensing/LicenseManager.kt` | Orquestador: `refresh()`, `currentState()`, `saveLicenseKey()`, `telemetrySummary()` |
| `app/src/test/java/com/ivanna/omega/licensing/` | `LicenseKeyUtilsTest`, `LicenseClientTest`, `LicenseManagerTest` (mock, sin red real) |
| `app/src/main/java/com/ivanna/omega/MainActivity.kt` | Gancho mínimo: `LicenseManager.refresh()` en `Dispatchers.IO` al arrancar |

## URL configurable

- Por defecto: `https://ivanna-license-server.onrender.com`
  (`LicenseConfig.DEFAULT_BASE_URL`).
- Se puede cambiar por preferencia sin recompilar
  (`LicenseConfig.KEY_BASE_URL_OVERRIDE`, vía
  `LicenseStore.setBaseUrlOverride`): para apuntar a un mock o al
  servidor local. En el emulador, el servidor local en `:8788` es
  `http://10.0.2.2:8788`.
- **Hoy la URL por defecto NO responde** (servidor sin desplegar):
  cada arranque cae en fail-open (ver abajo). Eso es intencional:
  la app no debe romperse por un servidor que aún no existe.

## Modo sin conexión (fail-open con caché)

Regla de oro: **NUNCA bloquear el audio por un fallo de licencia
o de red.**

| Situación | Estado resultante |
|---|---|
| Servidor: `valid:true` | `VALID` + se guarda la caché de gracia (plan, caducidad, dispositivos, instante) |
| Servidor: `valid:false` | `INVALID` (motivo) + **se borra la caché**. Un "no" explícito del servidor NO es fail-open: la caché no puede resucitar una licencia revocada o expirada |
| Sin red / timeout / respuesta corrupta + caché vigente | `VALID_OFFLINE` (acceso completo, marcado `offline=true`) |
| Sin red + caché mensual caducada | `INVALID` / `EXPIRED`, `offline=true` |
| Sin red + sin caché | `UNVALIDATED`, `offline=true` |
| Sin clave guardada | `UNVALIDATED` (sin llamada de red) |

La caché mensual solo vale mientras `expiresAt` no haya pasado;
la vitalicia vale siempre. `refresh()` es síncrono y nunca lanza
(cualquier fallo inesperado se trata como error de red); se llama
desde `Dispatchers.IO` en `MainActivity.onCreate`, sin bloquear la
UI ni el audio.

## Modo libre / limitado — definido CON EVIDENCIA

Búsqueda en el repo (2026-11-14): **no existe ninguna distinción
libre/pro ni paywall en la app**. Los únicos hits de "premium" son
nombres de perfiles de sonido (`VINILO PREMIUM`) y niveles de coste
de modelos de IA (`CostTier.PREMIUM`); no hay flags, preferencias
ni puertas que limiten funciones por pago. El único archivo con
"license" en el nombre es
`app/src/main/cpp/include/ivanna_npe_license.h`, que es licencia
del NPE, NO el sistema de compra.

Por tanto, el modo libre/limitado queda definido así:

- **Modo libre (sin licencia válida): la app sigue funcionando
  exactamente como hoy. Nada se bloquea, nada se degrada.**
  El estado queda como `UNVALIDATED` o `INVALID` (con su motivo) y
  se muestra así en la UI/telemetría.
- **Con licencia válida: acceso completo** (`VALID` o
  `VALID_OFFLINE`), mismo comportamiento que hoy más la marca de
  licencia válida.
- El estado es legible por la UI (`LicenseManager.currentState()`,
  `LicenseState.hasFullAccess`) y por la telemetría
  (`LicenseManager.telemetrySummary()` — acceso, plan,
  `devicesBound/devicesMax`, huella de la clave, última validación,
  `offline`, motivo — **sin la clave en claro**).

### PENDIENTE — decisión de producto de Esse

Qué se limita exactamente en modo libre (por ejemplo: ¿todo el DSP
sigue libre y lo de pago es otra cosa? ¿hay funciones que pasan a
requerir licencia?) **no está decidido y no se inventó ninguna
restricción**. Cuando Esse lo decida, se implementa como una puerta
que lea `LicenseState.hasFullAccess`, sin tocar el camino de audio
de emergencia: el audio base nunca depende de la licencia.

## Cómo verificarlo

### Con mock (hoy, sin servidor)

```bash
cd monetization/license-server
node --test test.mjs   # 8/8 del servidor, sin red
```

Tests de la app (requieren Android SDK / CI; en esta VM no hay):

```bash
./gradlew testDebugUnitTest --tests "com.ivanna.omega.licensing.*"
```

Cubren: éxito lifetime y monthly, `not_found`, `revoked`,
`expired`, `device_limit`, `bad_request`, sin red con caché vigente
(lifetime y monthly), sin red con caché mensual caducada, sin red
sin caché, respuesta corrupta (no-JSON, sin `valid`, plan
desconocido), normalización de la clave y que la telemetría nunca
expone la clave. Transporte mock inyectado; ninguna prueba toca la
red real.

### Cuando el servidor esté desplegado

1. Crear una licencia de prueba (admin, con `LICENSE_ADMIN_KEY`):
   `POST /v1/licenses/create {"plan":"lifetime","email":"prueba@ejemplo.com"}`.
2. En la app, guardar la clave con `LicenseManager.saveLicenseKey()`
   (hoy no hay pantalla para escribirla: es el siguiente paso de UI).
3. Arrancar la app con red: el log `IVANNA.License` debe decir
   `Licencia válida: plan=lifetime devices=1/3` y el estado debe ser
   `VALID`.
4. Apagar la red y rearrancar: estado `VALID_OFFLINE` (fail-open).
5. Revocar la clave (`POST /v1/licenses/revoke`) y rearrancar con
   red: estado `INVALID` / `revoked`, y la caché borrada.
6. Verificar que en todos los casos el audio sigue sonando.

## Riesgos y rollback

- **Servidor sin desplegar:** la validación real está pendiente;
  hoy todo arranque es fail-open/modo libre. Sin riesgo para el
  usuario: nada se bloquea.
- **Plan free de Render se duerme** tras 15 min sin uso (ver README
  del servidor): la primera validación tras el sueño puede tardar
  o dar timeout → fail-open con caché. El plan Starter ($7/mes) es
  lo recomendable; es decisión de Esse, no de este cambio.
- **Rollback:** borrar el paquete `licensing/` y el gancho de 8
  líneas de `MainActivity` devuelve la app a su estado anterior;
  ningún otro archivo depende de él. Las preferencias
  `ivanna_licensing` son independientes y no afectan a nada más.
- **La clave guardada** vive en SharedPreferences privado de la
  app. No es un secreto de la app (es la clave del usuario), pero
  si se quiere endurecer, el siguiente paso es guardarla cifrada
  (EncryptedSharedPreferences) — fuera del alcance de este cambio.
