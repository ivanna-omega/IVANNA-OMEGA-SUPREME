# Servidor de licencias de IVANNA

Servidor Node.js 20+ **sin dependencias** que vende y valida las licencias de
IVANNA OMEGA SUPREME. Las claves se guardan solo por su hash SHA-256: la
clave en claro se muestra una sola vez al crearla (o se re-deriva para las
de Stripe). Sin logs de claves, emails ni deviceIds en claro.

## Endpoints

| Método | Ruta | Auth | Qué hace |
|---|---|---|---|
| GET | `/healthz` | — | `{ok:true, service:"ivanna-license-server"}` |
| POST | `/v1/licenses/validate` | — (rate limit 60/min/IP) | `{licenseKey, deviceId}` → `{valid, plan, expiresAt, devicesBound, devicesMax}` o `{valid:false, reason}`. `reason`: `not_found`, `revoked`, `expired`, `device_limit`, `bad_request`. Vincula el dispositivo si hay hueco (máx. 3 por defecto). |
| POST | `/v1/licenses/create` | Bearer `LICENSE_ADMIN_KEY` | `{plan, email, months?, devicesMax?}` → `{licenseKey, plan, expiresAt, ...}`. La clave se devuelve **una sola vez**: guárdala y entrégala al comprador. `plan`: `lifetime` (sin caducidad) o `monthly` (30 días). |
| POST | `/v1/licenses/revoke` | Bearer `LICENSE_ADMIN_KEY` | `{licenseKey}` → `{revoked:true}`. |
| POST | `/v1/licenses/reissue` | Bearer `LICENSE_ADMIN_KEY` | `{email}` → re-emite la clave de una licencia creada por Stripe (se deriva del session id). Las manuales no se pueden re-emitir (409 `not_reissuable`): crea una nueva. |
| POST | `/v1/stripe/webhook` | Firma HMAC de Stripe | `checkout.session.completed` → crea la licencia. `invoice.payment_succeeded` → renueva 30 días. `customer.subscription.deleted` → caduca. Sin Stripe configurado responde **501** `stripe_not_configured` (el servidor no se rompe). |

## Store (dónde viven las licencias)

Interfaz única en `store.mjs` (`get/set/del/exists`):

- **Producción: Render Key Value (Redis).** Es el elegido porque el lookup
  de licencias es clave-valor puro (hash → registro), sin relaciones, y
  porque **Postgres gratis caduca a los 90 días en Render** (perder las
  licencias = los clientes pierden el acceso). Pon la URL interna de tu
  Key Value en `REDIS_URL`. Para miles de licencias (<1 KB cada una) el
  plan gratis sobra.
- **Desarrollo: memoria** (si no hay `REDIS_URL`). Aviso en el arranque:
  las licencias se pierden al reiniciar. No usar en producción.

## Variables de entorno

| Variable | Obligatoria | Qué es |
|---|---|---|
| `LICENSE_ADMIN_KEY` | **Sí** (≥32 caracteres, si no el servidor no arranca) | Clave del admin para crear/revocar/re-emitir. Genera una con: `node -e "console.log(require('crypto').randomBytes(32).toString('base64url'))"` |
| `LICENSE_KEY_SECRET` | No | Secreto para derivar las claves de Stripe. Si no se pone, se deriva de la admin key (rotar la admin key invalida las re-emisiones). |
| `REDIS_URL` | No (pero sí en producción) | URL del Key Value de Render. Sin ella usa memoria. |
| `STRIPE_SECRET_KEY` | No (sí para cobrar) | Clave secreta de Stripe (`sk_live_...`). |
| `STRIPE_WEBHOOK_SECRET` | No (sí para cobrar) | Secreto del webhook (`whsec_...`). Sin él, el webhook no verifica firmas y no se activa. |
| `STRIPE_PRICE_MAP` | No | JSON `{"price_xxx":"monthly", ...}` para inferir el plan desde el price id cuando el Checkout no lleva `metadata.plan`. |
| `PORT` | No (Render la inyecta; local: 8788) | Puerto. |

## Probar en local

```bash
cd monetization/license-server
node --test test.mjs          # tests (sin red)
LICENSE_ADMIN_KEY=$(node -e "console.log(require('crypto').randomBytes(32).toString('base64url'))") \
  node server.mjs             # arranca en :8788 (guarda la key en un papel: la necesitas)
```

En otra terminal (cambia `ADMIN` por tu key):

```bash
# Salud
curl -s http://localhost:8788/healthz

# Crear una licencia vitalicia (la clave sale UNA vez)
curl -s -X POST http://localhost:8788/v1/licenses/create \
  -H "Authorization: Bearer ADMIN" -H "Content-Type: application/json" \
  -d '{"plan":"lifetime","email":"comprador@ejemplo.com"}'

# Validar (deviceId = ANDROID_ID ya hasheado por la app)
curl -s -X POST http://localhost:8788/v1/licenses/validate \
  -H "Content-Type: application/json" \
  -d '{"licenseKey":"IVANNA-XXXX-XXXX-XXXX","deviceId":"abc123hash"}'

# Webhook sin Stripe → 501 claro
curl -s -X POST http://localhost:8788/v1/stripe/webhook \
  -H "Content-Type: application/json" -d '{}'
```

## Conectar Stripe (pasos para Esse)

1. Crea la cuenta en [stripe.com](https://stripe.com) y activa los pagos.
2. En Stripe Dashboard → Developers → API keys: copia la **Secret key**
   (`sk_live_...`) y ponla en Render como `STRIPE_SECRET_KEY`.
3. Crea un **Payment Link** (o Checkout) con `metadata.plan` = `lifetime`
   o `monthly`, y pon su URL en la landing (`STRIPE_CHECKOUT_URL` en
   `landing/script.js`).
4. En Stripe Dashboard → Developers → Webhooks → Add endpoint:
   URL `https://ivanna-license-server.onrender.com/v1/stripe/webhook`,
   eventos: `checkout.session.completed`, `invoice.payment_succeeded`,
   `customer.subscription.deleted`. Copia el **Signing secret**
   (`whsec_...`) y ponlo en Render como `STRIPE_WEBHOOK_SECRET`.
5. Prueba con una compra de prueba (modo test primero: `sk_test_...`).

## Despliegue (lo hace el agente principal con la key de Render)

Ver `monetization/README.md` (pasos por API y por dashboard). Resumen:
servicio web Docker desde `monetization/license-server/`, plan free para
empezar (**ojo**: el plan gratis se duerme tras 15 min sin uso; para un
servidor de licencias que debe responder siempre, el plan Starter de $7/mes
es lo recomendable). Variables: `LICENSE_ADMIN_KEY` (generar en el momento
del despliegue, no reutilizar la de local), `REDIS_URL` del Key Value.

## Lo que falta (honesto)

- **La app aún no valida licencias.** La integración en la app (llamar a
  `/v1/licenses/validate` al arrancar, guardar el estado, modo sin conexión
  con gracia) es trabajo de otro release. El contrato está en este README.
- **Entrega de la clave por email.** En el MVP, la compra por Stripe crea
  y registra la licencia, pero la clave no se envía sola: se re-emite con
  `/v1/licenses/reissue` (admin) y Esse la envía al comprador. Fase 2:
  envío automático por email (Resend/SendGrid).
- **Panel de admin.** No hay interfaz web; la gestión es por curl con la
  admin key. Fase 2.
- **Reembolsos.** Revocar es manual (`/v1/licenses/revoke`); Stripe no
  revoca solo al reembolsar. Fase 2.
