// licenses.mjs — Lógica pura de licencias de IVANNA (sin I/O, testeable).
// Una licencia es un registro guardado por el HASH SHA-256 de su clave.
// La clave en claro NUNCA se guarda: solo se muestra una vez al crearla
// (endpoint admin) o se re-deriva para las de Stripe (ver stripe.mjs).
import { randomBytes, createHash, createHmac } from 'node:crypto';

// Alfabeto Crockford sin caracteres confusos (sin 0/O, 1/I/L).
const ALPHABET = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';

// Formato de clave: IVANNA-XXXX-XXXX-XXXX (12 caracteres, 60 bits).
export function generateLicenseKey() {
  const bytes = randomBytes(12);
  let body = '';
  for (let i = 0; i < 12; i++) body += ALPHABET[bytes[i] % 32];
  return `IVANNA-${body.slice(0, 4)}-${body.slice(4, 8)}-${body.slice(8, 12)}`;
}

// Normaliza lo que escribe el usuario: mayúsculas, con o sin guiones/espacios.
// Devuelve la forma canónica o null si no tiene la forma válida.
export function normalizeLicenseKey(input) {
  if (typeof input !== 'string') return null;
  const clean = input.toUpperCase().replace(/[^A-Z0-9]/g, '');
  if (!/^IVANNA[ABCDEFGHJKMNPQRSTUVWXYZ23456789]{12}$/.test(clean)) return null;
  const body = clean.slice(6);
  return `IVANNA-${body.slice(0, 4)}-${body.slice(4, 8)}-${body.slice(8, 12)}`;
}

export function hashLicenseKey(canonicalKey) {
  return createHash('sha256').update(canonicalKey, 'utf8').digest('hex');
}

// Huella corta para logs (nunca la clave completa, nunca el email en claro).
export function fingerprint(value) {
  if (typeof value !== 'string' || value.length === 0) return 'n/a';
  const h = createHash('sha256').update(value, 'utf8').digest('hex');
  return h.slice(0, 8);
}

export function hashDeviceId(deviceId) {
  return createHash('sha256').update(String(deviceId), 'utf8').digest('hex');
}

export function hashEmail(email) {
  return createHash('sha256').update(String(email).trim().toLowerCase(), 'utf8').digest('hex');
}

// Clave determinista para licencias creadas por Stripe: se deriva del
// session id, así la clave se puede re-emitir (reissue) sin haberla guardado.
// `secret` es LICENSE_KEY_SECRET, o se deriva de la admin key (ver server.mjs).
export function deriveLicenseKey(secret, stripeSessionId) {
  const mac = createHmac('sha256', secret).update(`ivanna-license:${stripeSessionId}`, 'utf8').digest();
  let body = '';
  for (let i = 0; i < 12; i++) body += ALPHABET[mac[i] % 32];
  return `IVANNA-${body.slice(0, 4)}-${body.slice(4, 8)}-${body.slice(8, 12)}`;
}

// Planes conocidos. `expiresAt: null` = sin caducidad (licencia única).
export const PLANS = {
  lifetime: { devicesMax: 3, defaultMonths: null },
  monthly: { devicesMax: 3, defaultMonths: 1 },
};

export function isKnownPlan(plan) {
  return Object.prototype.hasOwnProperty.call(PLANS, plan);
}

// Crea el registro de una licencia (ya con la clave generada fuera).
// `opts`: { plan, email, months, devicesMax, stripeSessionId }
export function buildLicenseRecord(key, opts = {}) {
  const plan = isKnownPlan(opts.plan) ? opts.plan : 'lifetime';
  const devicesMax = Number.isInteger(opts.devicesMax) && opts.devicesMax > 0
    ? Math.min(opts.devicesMax, 10)
    : PLANS[plan].devicesMax;
  let expiresAt = null;
  if (typeof opts.expiresAt === 'string' && opts.expiresAt) {
    const d = new Date(opts.expiresAt);
    if (!Number.isNaN(d.getTime())) expiresAt = d.toISOString();
  } else if (Number.isInteger(opts.months) && opts.months > 0) {
    expiresAt = new Date(Date.now() + opts.months * 30 * 24 * 60 * 60 * 1000).toISOString();
  } else if (PLANS[plan].defaultMonths) {
    expiresAt = new Date(Date.now() + PLANS[plan].defaultMonths * 30 * 24 * 60 * 60 * 1000).toISOString();
  }
  return {
    keyHash: hashLicenseKey(key),
    plan,
    emailHash: opts.email ? hashEmail(opts.email) : null,
    expiresAt,
    devicesMax,
    devices: [], // hashes de deviceId vinculados
    revoked: false,
    stripeSessionId: opts.stripeSessionId || null,
    createdAt: new Date().toISOString(),
  };
}

// Valida una licencia contra su registro. Devuelve el resultado público.
// `deviceId` es lo que manda la app (ANDROID_ID ya hasheado por la app);
// aquí se vuelve a hashear antes de guardarlo: nunca se guarda en claro.
export function validateLicenseRecord(record, deviceId) {
  if (!record) return { valid: false, reason: 'not_found' };
  if (record.revoked) return { valid: false, reason: 'revoked' };
  if (record.expiresAt && new Date(record.expiresAt).getTime() <= Date.now()) {
    return { valid: false, reason: 'expired' };
  }
  const deviceHash = deviceId ? hashDeviceId(deviceId) : null;
  let newlyBound = false;
  if (deviceHash) {
    if (!record.devices.includes(deviceHash)) {
      if (record.devices.length >= record.devicesMax) {
        return { valid: false, reason: 'device_limit' };
      }
      record.devices.push(deviceHash);
      newlyBound = true;
    }
  }
  return {
    valid: true,
    plan: record.plan,
    expiresAt: record.expiresAt,
    devicesBound: record.devices.length,
    devicesMax: record.devicesMax,
    newlyBound,
  };
}

// Vista pública de un registro (sin hashes internos).
export function publicLicense(record) {
  if (!record) return null;
  return {
    plan: record.plan,
    expiresAt: record.expiresAt,
    devicesBound: record.devices.length,
    devicesMax: record.devicesMax,
    revoked: record.revoked,
    createdAt: record.createdAt,
  };
}
