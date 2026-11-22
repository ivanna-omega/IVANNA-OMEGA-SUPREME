# Accesibilidad e internacionalización — IVANNA-OMEGA-SUPREME

- **Versión del producto:** v2.5.445 (versionCode 736)
- **Fecha:** 2026-11-14 · **Base:** HEAD 74337b5a, rama `ivanna-real-completa`
- **Método:** revisión estática de `app/src/main/java` (338 archivos Kotlin)
  y `app/src/main/res/` en esta rama. **No se ejecutó una auditoría con
  TalkBack en dispositivo**: lo que sigue es el estado real del código,
  no una certificación. Lo no verificado figura como PENDIENTE.

## 1. Estado real — qué existe

### Etiquetas accesibles (contentDescription)

- **455 ocurrencias** de `contentDescription` en el código Kotlin de la app
  (búsqueda literal en `app/src/main/java`).
- **31 de ellas son `contentDescription = null`**: el patrón correcto para
  iconos puramente decorativos (p. ej. `EnginesStatusScreen.kt:360-371`,
  iconos con `tint` y `size(18.dp)` que acompañan a un texto que ya
  describe el elemento).
- El resto etiqueta iconos interactivos o informativos con texto en
  español: "Atrás" (`EnginesStatusScreen.kt:126`), "Volver"
  (`IvannaAssistantScreen.kt:374`), "Foto frontal para medir la cabeza"
  (`HeadMeasureScreen.kt:201`).
- **Límite:** varias etiquetas usan valores dinámicos que no describen la
  acción (`contentDescription = file.name`,
  `contentDescription = phase.name`): el nombre de un archivo o de una
  fase no es una etiqueta accesible útil. Esos casos deben revisarse uno
  por uno (ver §3).

### Regla aplicada en este documento

- **Controles interactivos** (botones, iconos clicables, sliders, switches):
  necesitan etiqueta accesible con la **acción** ("Atrás", "Reproducir",
  "Subir volumen"), no con el nombre del dato que muestran.
- **Iconos decorativos** (acompañan a un texto que ya dice lo mismo):
  `contentDescription = null` es correcto y es lo que ya hace parte del
  código.

### Tamaños táctiles y foco

- Existe al menos un objetivo táctil de 44 dp (`SpecSheet.kt:72`,
  `.size(44.dp)`). La guía de Material / Android recomienda 48×48 dp
  como mínimo; 44 dp está por debajo.
- **No se verificó** de forma sistemática el tamaño de todos los objetivos
  táctiles, el orden de foco, ni los estados de foco visible en cada
  pantalla. PENDIENTE: barrido con el verificador de accesibilidad
  (Accessibility Scanner) en dispositivo.

### Escalado de texto y contraste

- **Sin evidencia en el código** de manejo explícito de `fontScale`
  (escalado de texto del sistema) ni de pruebas de contraste por pantalla.
  Compose escala el texto con el sistema por defecto salvo que se fije
  tamaño en `sp` con `TextUnit` no escalable; no se auditó qué pantallas
  fijan tamaños.
- PENDIENTE: prueba con fuente al 130% y 200%, y revisión de contraste
  texto/fondo (tema Aurora Obsidiana: cian sobre oscuro) con medidor de
  contraste.

### TalkBack / lector de pantalla

- **Sin código específico de accesibilidad localizado**: no hay usos de
  `semantics { }`, `contentDescription` en `Modifier.semantics`, ni
  acciones personalizadas de accesibilidad en la búsqueda realizada.
  (La app depende de las etiquetas `contentDescription` de los
  composables de Material/Compose, que es el camino estándar.)
- **No se ha probado TalkBack en dispositivo en este inventario.**
  PENDIENTE: recorrido TalkBack completo de las pantallas principales
  (reproductor, ajustes DSP, asistente, Tidal).

## 2. Internacionalización (i18n) — estado real

- **La app está en español y solo en español.** No hay recursos de otro
  idioma.
- `app/src/main/res/values/strings.xml` contiene **una sola cadena**:
  `app_name` ("IVANNA-OMEGA-SUPREME"). **No se usa `stringResource` en
  ningún archivo Kotlin** (0 archivos con `stringResource`).
- Consecuencia: **todos los textos de la UI están hardcodeados en español
  dentro del código Kotlin**. Eso funciona hoy (el usuario es
  hispanohablante), pero:
  - No hay forma de traducir sin tocar código.
  - No hay `values-en/` ni ningún otro `values-<idioma>/`.
  - Los textos de `contentDescription` también están hardcodeados.
- **Estado: i18n NO implementada** (no es un bug para el usuario actual;
  es una deuda si el producto se vende fuera del mundo hispanohablante).
- Acción recomendada (no urgente): migrar textos a `strings.xml` por
  pantalla, empezando por los textos visibles y las etiquetas accesibles,
  y solo después añadir `values-en/`.

## 3. Qué falta (lista de trabajo)

| # | Falta | Prioridad | Verificación |
|---|---|---|---|
| A1 | Recorrido TalkBack en dispositivo (pantallas principales) | Alta | TalkBack real en Moto G85 |
| A2 | Barrido de objetivos táctiles < 48 dp | Alta | Accessibility Scanner |
| A3 | Revisar etiquetas dinámicas (`file.name`, `phase.name`) y poner la acción | Media | Revisión por pantalla |
| A4 | Prueba de fuente 130%/200% y de contraste (Aurora Obsidiana) | Media | Dispositivo + medidor de contraste |
| A5 | Orden de foco y foco visible por pantalla | Media | Navegación por teclado/switch |
| A6 | Migrar textos hardcodeados a `strings.xml` | Baja | 0 `stringResource` hoy → >0 por pantalla |
| A7 | `values-en/` (solo tras A6) | Baja | Build con locale en inglés |

## 4. Criterio de aceptación

- A1–A2 superados en dispositivo y documentados (qué pantalla, qué hallazgo,
  qué fix).
- Ningún control interactivo sin etiqueta accesible (barrido estático:
  todo `IconButton`/`clickable` con `contentDescription` no nulo o con
  texto visible asociado).
- Todos los iconos decorativos con `contentDescription = null` (ya es el
  patrón mayoritario: 31 casos).
