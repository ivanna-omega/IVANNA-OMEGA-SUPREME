// server.mjs — Servidor de licencias de IVANNA (Node.js puro, sin dependencias).
// Endpoints:
//   GET  /healthz                     → salud
//   POST /v1/licenses/validate        → {licenseKey, deviceId}
//   POST /v1/licenses/create          → admin, {plan, email, months?, devicesMax?}
//   POST /v1/licenses/revoke          → admin, {licenseKey}
//   POST /v1/licenses/reissue         → admin, {email}
//   POST /v1/stripe/webhook           → webhook de Stripe (501 si no configurado)
// Seguridad: las claves se guardan solo por su hash SHA-256; el admin se
// autentica con LICENSE_ADMIN_KEY (Bearer, comparación en tiempo constante);
// rate limit por IP; nunca se loguean claves, emails ni deviceIds en claro.
// Requiere LICENSE_ADMIN_KEY (>= 32 caracteres) o no arranca.
import http from 'node:http';
import { timingSafeEqual, createHash } from 'node:crypto';
import {
  generateLicenseKey,
  normalizeLicenseKey,
  hashLicenseKey,
  fingerprint,
  hashEmail,
  deriveLicenseKey,
  buildLicenseRecord,
  validateLicenseRecord,
  publicLicense,
  isKnownPlan,
} from './licenses.mjs';
import { createStore, licenseKey, emailIndexKey } from './store.mjs';
import {
  isStripeConfigured,
  verifyStripeSignature,
  checkoutDetails,
  RENEWAL_MONTHS,
} from './stripe.mjs';

const PORT = Number(process.env.PORT || 8788);
const ADMIN_KEY = process.env.LICENSE_ADMIN_KEY || '';
if (ADMIN_KEY.length < 32) {
  console.error('LICENSE_ADMIN_KEY debe tener al menos 32 caracteres; el servidor no arranca.');
  console.error('Genera una con: node -e "console.log(require(\'crypto\').randomBytes(32).toString(\'base64url\'))"');
  process.exit(1);
}
// Secreto para derivar las claves de Stripe. Si no se pone uno propio, se
// deriva de la admin key (rotar la admin key invalida las re-emisiones).
const KEY_SECRET = process.env.LICENSE_KEY_SECRET ||
  createHash('sha256').update(`ivanna-key-derivation:${ADMIN_KEY}`, 'utf8').digest('hex');

let PRICE_MAP = {};
try {
  PRICE_MAP = process.env.STRIPE_PRICE_MAP ? JSON.parse(process.env.STRIPE_PRICE_MAP) : {};
} catch {
  console.warn('[stripe] STRIPE_PRICE_MAP no es JSON válido; se ignora.');
}

const MAX_BODY = 16 * 1024;
const store = createStore();

const buckets = new Map();
function rateAllowed(ip, limit, windowMs = 60_000) {
  const now = Date.now();
  const k = `${ip}:${limit}`;
  let b = buckets.get(k);
  if (!b || now - b.start >= windowMs) {
    b = { start: now, count: 0 };
    buckets.set(k, b);
  }
  b.count += 1;
  return b.count <= limit;
}

function json(res, status, data) {
  res.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'cache-control': 'no-store',
    'x-content-type-options': 'nosniff',
    'referrer-policy': 'no-referrer',
  });
  res.end(JSON.stringify(data));
}

function adminAuthorized(req) {
  const header = req.headers.authorization || '';
  if (!header.startsWith('Bearer ')) return false;
  const candidate = Buffer.from(header.slice(7));
  const expected = Buffer.from(ADMIN_KEY);
  return candidate.length === expected.length && timingSafeEqual(candidate, expected);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (chunk) => {
      size += chunk.length;
      if (size > MAX_BODY) {
        reject(new Error('body_too_large'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

async function readJson(req) {
  const raw = await readBody(req);
  if (raw.length === 0) return {};
  try {
    return JSON.parse(raw.toString('utf8'));
  } catch {
    throw new Error('bad_json');
  }
}

function clientIp(req) {
  const fwd = req.headers['x-forwarded-for'];
  if (typeof fwd === 'string') return fwd.split(',')[0].trim();
  return req.socket?.remoteAddress || 'unknown';
}

// --- Acceso al store (la licencia vive por su hash; el email por su hash) ---
async function loadLicense(keyHash) {
  const raw = await store.get(licenseKey(keyHash));
  return raw ? JSON.parse(raw) : null;
}
async function saveLicense(record) {
  await store.set(licenseKey(record.keyHash), JSON.stringify(record));
  if (record.emailHash) {
    await store.set(emailIndexKey(record.emailHash), record.keyHash);
  }
}

async function handleValidate(req, res) {
  const body = await readJson(req);
  const canonical = normalizeLicenseKey(body.licenseKey);
  if (!canonical || typeof body.deviceId !== 'string' || body.deviceId.length === 0) {
    return json(res, 400, { valid: false, reason: 'bad_request' });
  }
  const record = await loadLicense(hashLicenseKey(canonical));
  const result = validateLicenseRecord(record, body.deviceId);
  if (result.valid && result.newlyBound) {
    await saveLicense(record); // persiste el dispositivo vinculado
  }
  console.log(`[validate] key=${fingerprint(canonical)} valid=${result.valid} reason=${result.reason || 'ok'}`);
  if (!result.valid) return json(res, 200, { valid: false, reason: result.reason });
  const { newlyBound, ...pub } = result;
  return json(res, 200, pub);
}

async function handleCreate(req, res) {
  if (!adminAuthorized(req)) return json(res, 401, { error: 'unauthorized' });
  const body = await readJson(req);
  if (!isKnownPlan(body.plan)) return json(res, 400, { error: 'unknown_plan' });
  if (typeof body.email !== 'string' || !body.email.includes('@')) {
    return json(res, 400, { error: 'bad_email' });
  }
  const key = generateLicenseKey();
  const record = buildLicenseRecord(key, {
    plan: body.plan,
    email: body.email,
    months: body.months,
    devicesMax: body.devicesMax,
  });
  await saveLicense(record);
  console.log(`[create] plan=${record.plan} emailFp=${fingerprint(body.email)} expiresAt=${record.expiresAt}`);
  // La clave en claro se devuelve UNA sola vez: el admin debe entregarla.
  return json(res, 201, { licenseKey: key, ...publicLicense(record) });
}

async function handleRevoke(req, res) {
  if (!adminAuthorized(req)) return json(res, 401, { error: 'unauthorized' });
  const body = await readJson(req);
  const canonical = normalizeLicenseKey(body.licenseKey);
  if (!canonical) return json(res, 400, { error: 'bad_key' });
  const record = await loadLicense(hashLicenseKey(canonical));
  if (!record) return json(res, 404, { error: 'not_found' });
  record.revoked = true;
  await saveLicense(record);
  console.log(`[revoke] key=${fingerprint(canonical)}`);
  return json(res, 200, { revoked: true });
}

async function handleReissue(req, res) {
  if (!adminAuthorized(req)) return json(res, 401, { error: 'unauthorized' });
  const body = await readJson(req);
  if (typeof body.email !== 'string' || !body.email.includes('@')) {
    return json(res, 400, { error: 'bad_email' });
  }
  const keyHash = await store.get(emailIndexKey(hashEmail(body.email)));
  if (!keyHash) return json(res, 404, { error: 'not_found' });
  const record = await loadLicense(keyHash);
  if (!record || !record.stripeSessionId) {
    // Solo las licencias de Stripe se pueden re-derivar; las manuales se
    // entregaron una sola vez al crearlas (si se pierde, crear una nueva).
    return json(res, 409, { error: 'not_reissuable' });
  }
  const key = deriveLicenseKey(KEY_SECRET, record.stripeSessionId);
  console.log(`[reissue] emailFp=${fingerprint(body.email)}`);
  return json(res, 200, { licenseKey: key, ...publicLicense(record) });
}

async function handleStripeWebhook(req, res) {
  if (!isStripeConfigured()) {
    return json(res, 501, {
      error: 'stripe_not_configured',
      message: 'Pon STRIPE_SECRET_KEY y STRIPE_WEBHOOK_SECRET en Render para activar los pagos. Ver README.',
    });
  }
  const raw = await readBody(req);
  let event;
  try {
    event = verifyStripeSignature(raw.toString('utf8'), req.headers['stripe-signature'], process.env.STRIPE_WEBHOOK_SECRET);
  } catch (err) {
    console.log(`[stripe] firma inválida: ${err.message}`);
    return json(res, 400, { error: 'invalid_signature' });
  }

  if (event.type === 'checkout.session.completed') {
    const { email, plan, sessionId } = checkoutDetails(event, PRICE_MAP);
    if (!email || !sessionId) return json(res, 400, { error: 'missing_data' });
    const key = deriveLicenseKey(KEY_SECRET, sessionId);
    const record = buildLicenseRecord(key, { plan, email, stripeSessionId: sessionId });
    await saveLicense(record);
    console.log(`[stripe] licencia creada plan=${plan} emailFp=${fingerprint(email)} session=${sessionId.slice(0, 12)}...`);
    return json(res, 200, { received: true, licenseCreated: true });
  }

  if (event.type === 'invoice.payment_succeeded') {
    const sub = event.data?.object || {};
    const email = sub.customer_email || null;
    if (email) {
      const keyHash = await store.get(emailIndexKey(hashEmail(email)));
      const record = keyHash ? await loadLicense(keyHash) : null;
      if (record && record.plan === 'monthly' && !record.revoked) {
        const base = Math.max(new Date(record.expiresAt || 0).getTime(), Date.now());
        record.expiresAt = new Date(base + RENEWAL_MONTHS * 30 * 24 * 60 * 60 * 1000).toISOString();
        await saveLicense(record);
        console.log(`[stripe] suscripción renovada emailFp=${fingerprint(email)} hasta ${record.expiresAt}`);
      }
    }
    return json(res, 200, { received: true });
  }

  if (event.type === 'customer.subscription.deleted') {
    const sub = event.data?.object || {};
    const email = sub.customer_email || null;
    if (email) {
      const keyHash = await store.get(emailIndexKey(hashEmail(email)));
      const record = keyHash ? await loadLicense(keyHash) : null;
      if (record) {
        record.expiresAt = new Date().toISOString();
        await saveLicense(record);
        console.log(`[stripe] suscripción cancelada emailFp=${fingerprint(email)}`);
      }
    }
    return json(res, 200, { received: true });
  }

  return json(res, 200, { received: true, ignored: event.type });
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url || '/', 'http://localhost');
    const ip = clientIp(req);
    if (req.method === 'GET' && url.pathname === '/healthz') {
      return json(res, 200, { ok: true, service: 'ivanna-license-server' });
    }
    if (req.method === 'POST' && url.pathname === '/v1/licenses/validate') {
      if (!rateAllowed(ip, 60)) return json(res, 429, { error: 'rate_limited' });
      return await handleValidate(req, res);
    }
    if (req.method === 'POST' && url.pathname === '/v1/licenses/create') {
      if (!rateAllowed(ip, 20)) return json(res, 429, { error: 'rate_limited' });
      return await handleCreate(req, res);
    }
    if (req.method === 'POST' && url.pathname === '/v1/licenses/revoke') {
      if (!rateAllowed(ip, 20)) return json(res, 429, { error: 'rate_limited' });
      return await handleRevoke(req, res);
    }
    if (req.method === 'POST' && url.pathname === '/v1/licenses/reissue') {
      if (!rateAllowed(ip, 20)) return json(res, 429, { error: 'rate_limited' });
      return await handleReissue(req, res);
    }
    if (req.method === 'POST' && url.pathname === '/v1/stripe/webhook') {
      if (!rateAllowed(ip, 60)) return json(res, 429, { error: 'rate_limited' });
      return await handleStripeWebhook(req, res);
    }
    return json(res, 404, { error: 'not_found' });
  } catch (err) {
    if (err.message === 'body_too_large') return json(res, 413, { error: 'body_too_large' });
    if (err.message === 'bad_json') return json(res, 400, { error: 'bad_json' });
    console.error('[server] error:', err.message);
    return json(res, 500, { error: 'internal' });
  }
});

server.listen(PORT, () => {
  console.log(`[license-server] escuchando en :${PORT} (store: ${process.env.REDIS_URL ? 'Redis' : 'memoria'})`);
});
