# Modelo de amenazas — IVANNA-OMEGA-SUPREME

- **Versión del producto:** v2.5.445 (versionCode 736)
- **Fecha:** 2026-11-14 · **Base:** HEAD 74337b5a, rama `ivanna-real-completa`
- **Alcance:** app Android (Kotlin/Compose + JNI/C++ NDK), efecto AudioFlinger
  (Ruta B), daemon root `ivanna_asrcd`, módulo Magisk, archivos de audio y
  datasets SOFA, socket abstracto, WebView, actualizador, LLM (OmniRoute /
  Gemini) y autonomía (consejo multi-modelo + motor de evolución).
- **Lo que este documento NO contiene:** ningún valor sensible (claves,
  tokens, secretos, hashes de credenciales). Las referencias a credenciales
  son por nombre y ubicación de almacenamiento, nunca por valor.
- **Postura:** IVANNA es software propietario de Luis Uriel Pimentel Pérez
  (repo público sin LICENSE, NO open source). La amenaza principal no es
  solo el robo de código: es que un atacante altere el audio, el daemon root
  o la cadena de actualización en un teléfono rooteado.

## 1. Supuestos y límites del modelo

1. El teléfono de desarrollo es un Moto G85 rooteado con Magisk. Un
   atacante con root en el dispositivo ya puede leer/escribir casi todo; el
   modelo se centra en atacantes que NO tienen root (app maliciosa, red,
   suministro) y en contener el daño si el root se compromete.
2. `audioserver` corre sin privilegios de root y NO puede escribir en
   `/data/adb` (0700 root) ni en `/data/local/tmp` (verificado: el beacon
   ausente no prueba por sí solo que no haya procesado).
3. La app pide permiso de superusuario para el daemon y el diagnóstico; sin
   él solo funciona la Ruta A dentro de la app.
4. No hay ventas (0 vendidos, $0) ni cobros activos; Stripe no está
   habilitada para cobros. La monetización (servidor de licencias,
   `monetization/`) no está desplegada ni validada por la app.

## 2. Activos

| Activo | Dónde vive | Impacto si se compromete |
|---|---|---|
| Código DSP propietario (C++/Kotlin) | Repo público (visible, sin LICENSE) | Copia/competencia; el repo público ya expone el código — la protección real es ofuscación + integridad, no secreto |
| Clave de firma del APK (keystore) | Fuera del repo (debe seguir así) | Un atacante con ella firma APKs maliciosos indistinguibles |
| Token/sesión TIDAL del usuario | `TidalSessionStore.kt` (app) | Acceso a la cuenta TIDAL del usuario; refresh con client_id PKCE de la app de TIDAL |
| Credenciales Firebase / Google | `google-services.json` (config pública) + Auth | Ver §3: la config pública NO es un secreto |
| Daemon root `ivanna_asrcd` | `/data/adb/ivanna_omega`, socket `@omega_daemon_socket` | Ejecución como root; compromiso = control del teléfono |
| Estado de fusión / reloj ASRC | `/data/adb/ivanna_omega/`, shm | Audio alterado, denegación de servicio de audio |
| Datasets SOFA/HRTF (694 archivos) | `app/src/main/assets`, módulo Magisk | Envenenamiento de datasets → HRTF incorrecta (sonido wrong, no solo robo) |
| Canal LLM (OmniRoute/Gemini) | VM + túnel público localhost.run | Prompt injection hacia la app; órdenes DSP falsas |
| Motor de evolución / consejo | `tools/evolution/`, GitHub PAT | Reescritura de código maliciosa publicada como release |
| Diagnóstico vivo | Puente Floot/Render | Fuga de telemetría; órdenes remotas si el puente se compromete |

## 3. Firebase: configuración pública ≠ credencial secreta

La clave de cliente de Firebase (`apiKey` en `google-services.json`) es
**configuración pública, no un secreto**: identifica el proyecto, no
autentica nada por sí sola. Su sola presencia en el repo o en el APK **NO
prueba filtración** ni acceso no autorizado. Lo que sí protege (y debe
verificarse, no asumirse):

- **Reglas de Firestore / Storage:** denegar por defecto; cada colección con
  reglas que exijan autenticación y autoricen solo al dueño de los datos.
  Una regla abierta (`allow read, write: if true`) con la clave pública sí
  es una filtración real.
- **App Check:** la app instala `DebugAppCheckProviderFactory`
  (`CloudSyncManager.kt:120-124`) — proveedor de DEBUG. En producción debe
  usarse el proveedor de release (Play Integrity / SafetyNet); el proveedor
  debug en un build release anula la atestación.
- **Cuotas y alertas:** límites de uso y alertas de facturación en la
  consola Firebase; una clave pública abusada se detecta por cuota, no por
  secreto.
- **Auth:** Firebase Auth (`firebase-auth`, BOM 35.1.0) — las cuentas se
  protegen con reglas, no con la apiKey.

**Acción pendiente:** auditar reglas de Firestore/Storage y confirmar que
ningún build release usa el proveedor debug de App Check. Sin esa auditoría,
el estado es PENDIENTE, no "seguro".

## 4. Amenazas por superficie

### 4.1 App Android (Kotlin/Compose)

| Amenaza | Vector | Mitigación actual | Estado |
|---|---|---|---|
| Repackaging / APK modificado | APK público en releases | Firma del instalador (misma keystore); R8 + ofuscación; anti-debug/anti-tamper/anti-hook (Fases 1–2 completadas) | Mitigado parcialmente; la imposibilidad absoluta no existe en un dispositivo que el atacante controla |
| Extracción de la sesión TIDAL | Backup / root / depuración | Sin evidencia de cifrado del store en este inventario | PENDIENTE: verificar `TidalSessionStore` (EncryptedSharedPreferences o equivalente) |
| Permiso de superusuario abusado | App con root pide `su` | Solo la app IVANNA lo usa para daemon/diagnóstico; sin root, Ruta A sigue funcionando | Aceptado con límite: la app nunca debe ejecutar comandos `su` arbitrarios recibidos de red o del LLM |
| Intents / deep links | Otras apps | Sin deep links documentados en este inventario | PENDIENTE: auditar `AndroidManifest.xml` (exported, intent-filters) |

### 4.2 JNI / C++ NDK (el código que más daño puede hacer)

- **Superficie:** 13+ archivos JNI (`app/src/main/cpp/jni/`), puentes
  `*_controls_bridge.cpp` / `*_globals_effect.cpp` con atómicos compartidos
  entre la app y audioserver.
- **Amenazas:** desbordamiento por tamaños de buffer no validados desde
  Kotlin; uso tras liberación en el ciclo de vida del efecto; condiciones de
  carrera en los atómicos `g_*` (dobles con CAS loop, no `atomic_fetch_add`
  — ver auditoría ASRC v6, H-2); `LOG` en el hilo de tiempo real
  (`omega_process`) como categoría latente de Fase 0.
- **Mitigaciones verificadas:** auditoría ASRC v6 (23 hallazgos, todos
  corregidos y re-auditados: `pread()` en vez de dereferencia de
  `base+offset`, `fstat` ≥1008 B antes de `mmap`, seqlock por `sequence`,
  resolución eager en `pal_stream_open`, nunca `dlopen` en hilo de audio).
- **Regla durable:** la creación del efecto debe ser mínima y segura; el
  motor pesado se crea LAZY en el primer bloque de audio, nunca en
  `omega_create_effect` ni en `dlopen` (4 bricks por constructor con I/O en
  audioserver → `std::terminate` → bootloop). Ver `~/AGENTS.md`.
- **Pendiente:** fuzzing sistemático de la frontera JNI (entradas Kotlin →
  nativo) y eliminación del LOG en RT.

### 4.3 Archivos de audio y datasets SOFA

- **Amenaza:** un archivo `.sofa` / `.wav` / RIR manipulado (en el APK, en
  Download, o en el módulo) alimenta al loader con dimensiones o tasas
  falsas → lectura fuera de límites, HRTF incorrecta, sonido degradado.
- **Mitigación actual:** `SofaHRTFLoader::load()` verifica firma HDF5
  (8 bytes) + tamaño ≥512 B; inventario SOFA con SHA-256 por archivo
  (`scripts/inventory_sofa_metadata.py`, expected-count 694); re-encode
  float32 + gzip-9 verificado 239/239 legible (subconjunto del módulo).
- **Límite honesto:** un HDF5 legible no garantiza semántica SOFA/AES69
  completa; la validación semántica por convención es una puerta pendiente.
  Las licencias deben revisarse por conjunto antes de redistribuir.
- **Pendiente:** validar dimensiones/tasas del SOFA contra lo declarado
  antes de usarlo en el path de audio (fail-closed si no coincide).

### 4.4 Socket abstracto y memoria compartida (daemon ↔ efecto)

- **Superficie:** socket abstracto `@omega_daemon_socket` (AF_UNIX,
  `ivanna_daemon.cpp:137-142`), archivos en `/data/adb/ivanna_omega/`
  (0700 root), shm de fusión con seqlock.
- **Amenazas:** cualquier proceso con el mismo UID/namespace puede conectar
  al socket abstracto (no hay autenticación por credencial en el socket
  abstracto de Android: el nombre es global al namespace); un cliente falso
  puede inyectar comandos al daemon root o leer el estado de fusión.
- **Mitigaciones:** el daemon es root y su directorio es 0700; el seqlock
  usa `sequence` con retry (H-1); el supervisor relanza el daemon si corre
  sin socket.
- **Pendiente (alto):** autenticar al peer del socket (credenciales
  `SO_PEERCRED`: UID/PID del cliente) y rechazar todo lo que no sea
  audioserver/app IVANNA; documentar el protocolo de comandos del daemon
  (hoy 28/28 tests en host, pero sin especificación de autorización).

### 4.5 Daemon root y módulo Magisk

- **Amenaza máxima:** el daemon corre como root. Un bug (p. ej. el
  `mmap` con offset 0x2ec180 si una OTA mueve la función — ya mitigado con
  `pread()`) o un comando mal validado = ejecución arbitraria como root.
- **Historial que lo demuestra:** 7 resets de fábrica; el wrapper del HAL
  (v2.5.403) causó bootloop; el constructor del FusionEngine en
  `omega_create_effect` tumbó audioserver 4 veces.
- **Mitigaciones:** kill switches (`pal_asr.off` daemon v2,
  `pal_asrc.off` write-stage v1) para bypass inmediato sin reiniciar;
  creación lazy; auditoría v6 completa.
- **Regla:** nadie instala nada en el teléfono ni reinicia nada sin luz
  verde explícita de Esse; el wrapper del HAL NO se reintenta; el zip
  v2.5.403 NO se instala.
- **Pendiente:** lista cerrada de comandos del daemon con validación de
  argumentos y longitud máxima; ningún `system()`/`popen()` con entrada
  externa.

### 4.6 WebView

- **Superficie:** WebViews en `TidalScreen.kt`, `SpatialAudioPanel.kt`,
  `FotoHoloPlaceImage.kt`, `FotoHoloTelemetryBridge.kt` (búsqueda de
  "WebView" en `app/src/main/java`).
- **Amenazas:** JavaScript habilitado + puente JS → ejecución en la app;
  carga de contenido no confiable; el registro de Gmail 2026-11-12 ya
  mostró un WebView que no avanzaba ("Continuar" sin respuesta).
- **Pendiente:** verificar en cada WebView: JavaScript solo si es
  imprescindible, sin `addJavascriptInterface` salvo justificación,
  sin carga de `file://` cruzada, y navegación limitada a dominios
  permitidos. Estado actual: PENDIENTE de auditoría (no verificado en
  este inventario).

### 4.7 Actualizador (AppUpdateChecker) — autenticidad e integridad

- **Estado actual (`AppUpdateChecker.kt`):** descarga el APK del release de
  GitHub, verifica tamaño completo y SHA256 **solo si** el release publica
  `SHA256SUMS.txt`; si no hay SHA256 conocido, **no se exige**. La firma
  del APK la verifica el instalador del sistema (misma keystore).
- **Por qué eso no basta solo:** un SHA256 publicado en el mismo origen
  (el mismo release de GitHub) prueba integridad contra errores de
  descarga, pero **no prueba autenticidad** si el origen se compromete
  (atacante que controla el release publica su APK con su SHA256).
- **Requisito:** verificación de firma independiente del origen: firma
  detached (p. ej. minisign/signify o GPG) del APK + `SHA256SUMS.txt`,
  verificada contra una **clave pública anclada en la app** (no descargada
  del mismo release). El instalador del sistema sigue siendo la segunda
  barrera, no la única.
- **Adicional:** el canal debe ser HTTPS con el host fijado
  (`api.github.com` / `github.com`); rechazar redirecciones a otros hosts;
  rechazar APKs cuyo `versionCode` sea menor que el instalado (anti-downgrade).
- **Estado:** INTEGRADA con límite conocido (ver `cap-023`); la firma
  independiente es PENDIENTE. Ningún APK debe ofrecerse como "verificado"
  sin SHA256 conocido.

### 4.8 LLM y autonomía (OmniRoute, Gemini, consejo, evolución)

- **Amenazas:**
  - *Prompt injection:* contenido de TIDAL, de una página, de un archivo
    de audio con metadatos, o del puente de diagnóstico, que instruye a
    Ami o al consejo a cambiar parámetros DSP, desactivar IntegrityGate o
    publicar código.
  - *Órdenes DSP falsas:* el LLM devuelve un ajuste que la app aplica sin
    validar rango ni origen.
  - *Evolución comprometida:* el PAT de GitHub o el gateway OmniRoute
    comprometidos permiten publicar un release malicioso; la CI no corría
    los tests Python de evolución al publicar la 445 (por eso `council.py`
    faltó sin que nadie lo notara).
  - *Túnel público:* la URL de localhost.run rota y es pública; cualquiera
    que la conozca puede intentar hablar con el gateway.
- **Mitigaciones y reglas vigentes:**
  - Contención fail-closed: la auto-reparación solo actúa dentro del
    dominio IVANNA; fuera de él se bloquea y se reporta. Prohibido:
    deshabilitar IntegrityGate, tocar credenciales, comprar.
  - Consejo: sin consenso → RECHAZADO; máx. 3 rondas; el consejo no aplica
    nada, devuelve el parche.
  - Los modelos élite de pago no se usan sin cuentas; no se roban claves
    ni se burla autenticación.
  - La URL del túnel es temporal; pendiente dominio fijo.
- **Pendiente:** validación de rango/origen en todo ajuste DSP que venga
  del LLM (allowlist de parámetros + límites); autenticación del gateway
  (token) para que la URL pública no sea acceso abierto; incluir los tests
  Python de evolución en el gate de CI.

### 4.9 TIDAL (vía no oficial)

- La reproducción completa usa `playbackinfopostpaywall`, fuera de
  términos y revocable. El token del usuario y el `client_id` PKCE de la
  app de TIDAL viajan por ese camino; si TIDAL revoca el endpoint o exige
  `client_secret` en el refresh, la sesión caduca (~1 semana) y exige
  re-login.
- No inventar acuerdos: no hay registro en developer.tidal.com, no hay
  client ID propio, no hay acuerdo comercial (probabilidad ~1 de 10).
  La integración autorizada está BLOQUEADA (ver `cap-025`).
- El token de sesión debe almacenarse cifrado y nunca en logs.

### 4.10 Diagnóstico vivo y telemetría

- Endpoints Floot/Render (`LiveDiagnosticConnector.kt`); la URL + token
  del puente aún no están configurados en la app y el watcher (cada 5 min)
  no está activo.
- El token del puente es una credencial: va en almacenamiento seguro, nunca
  en el repo ni en logs. El puente solo acepta diagnósticos autenticados;
  un puente sin autenticación permitiría inyectar diagnósticos falsos que
  disparen auto-reparaciones.

## 5. Tabla de riesgos priorizada

| # | Riesgo | Prob. | Impacto | Estado |
|---|---|---|---|---|
| R1 | Socket del daemon sin autenticación de peer | Media | Crítico (root) | PENDIENTE |
| R2 | Actualizador sin firma independiente del origen | Baja | Crítico (APK falso) | PENDIENTE |
| R3 | WebView sin auditar (JS / puentes) | Media | Alto | PENDIENTE |
| R4 | Reglas Firestore / App Check debug en release | Desconocida | Alto | PENDIENTE (auditar) |
| R5 | Prompt injection → ajuste DSP o evolución | Media | Alto | PENDIENTE (allowlist + auth gateway) |
| R6 | Sesión TIDAL sin cifrado verificado | Baja | Medio | PENDIENTE |
| R7 | Daemon root con bug (historial: 7 resets) | Media | Crítico | Mitigado parcialmente (auditoría v6, kill switch, lazy) |
| R8 | SOFA envenenado / semántica no validada | Baja | Medio | PENDIENTE (validación semántica) |
| R9 | Túnel público sin autenticación | Media | Medio | PENDIENTE (dominio fijo + token) |
| R10 | Tests de evolución fuera del gate de CI | Probado (445) | Alto | PENDIENTE (incluir en CI) |

## 6. Principios de respuesta

1. **Fail-closed siempre:** si una verificación falta (sin SHA256, sin
   firma, sin autenticación), no se instala, no se aplica, no se publica.
2. **Kill switch antes que reinicio:** todo componente con privilegios
   tiene bypass inmediato sin reiniciar (`pal_asr.off`, `pal_asrc.off`).
3. **Nada sin luz verde en el teléfono:** sin installs, reboots,
   reinicios de audioserver ni cambios de módulos sin Esse.
4. **La evidencia, no la confianza:** cada mitigación cita su verificación;
   lo no verificado figura como PENDIENTE, no como "seguro".
