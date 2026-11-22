// test.mjs — Tests del servidor de licencias (node --test test.mjs).
// No tocan la red ni el store real: prueban la lógica pura y el webhook.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import {
  generateLicenseKey,
  normalizeLicenseKey,
  hashLicenseKey,
  deriveLicenseKey,
  buildLicenseRecord,
  validateLicenseRecord,
  isKnownPlan,
} from './licenses.mjs';
import { MemoryStore } from './store.mjs';
import { verifyStripeSignature, checkoutDetails } from './stripe.mjs';

test('la clave tiene formato IVANNA-XXXX-XXXX-XXXX y es única', () => {
  const a = generateLicenseKey();
  const b = generateLicenseKey();
  assert.match(a, /^IVANNA-[A-Z2-9]{4}-[A-Z2-9]{4}-[A-Z2-9]{4}$/);
  assert.notEqual(a, b);
  assert.equal(normalizeLicenseKey(a.toLowerCase().replace(/-/g, ' ')), a);
  assert.equal(normalizeLicenseKey('basura'), null);
});

test('el hash es determinista y no revela la clave', () => {
  const key = generateLicenseKey();
  assert.equal(hashLicenseKey(key), hashLicenseKey(key));
  assert.ok(!hashLicenseKey(key).includes(key.slice(7, 11)));
});

test('validar: no existe, válida, límite de dispositivos, caducada, revocada', () => {
  const key = generateLicenseKey();
  const rec = buildLicenseRecord(key, { plan: 'lifetime', email: 'a@b.com' });
  assert.equal(rec.expiresAt, null);
  assert.equal(rec.devicesMax, 3);

  assert.deepEqual(validateLicenseRecord(null, 'd1'), { valid: false, reason: 'not_found' });

  // Vincula dispositivos hasta el límite.
  assert.equal(validateLicenseRecord(rec, 'd1').valid, true);
  assert.equal(validateLicenseRecord(rec, 'd2').valid, true);
  assert.equal(validateLicenseRecord(rec, 'd3').valid, true);
  assert.deepEqual(validateLicenseRecord(rec, 'd4'), { valid: false, reason: 'device_limit' });
  // Un dispositivo ya vinculado sigue pasando.
  assert.equal(validateLicenseRecord(rec, 'd1').valid, true);
  assert.equal(rec.devices.length, 3);

  const expired = buildLicenseRecord(generateLicenseKey(), { plan: 'monthly', email: 'c@d.com' });
  expired.expiresAt = new Date(Date.now() - 1000).toISOString();
  assert.deepEqual(validateLicenseRecord(expired, 'd1'), { valid: false, reason: 'expired' });

  const revoked = buildLicenseRecord(generateLicenseKey(), { plan: 'lifetime', email: 'e@f.com' });
  revoked.revoked = true;
  assert.deepEqual(validateLicenseRecord(revoked, 'd1'), { valid: false, reason: 'revoked' });
});

test('el plan mensual caduca a 30 días y el vitalicio no caduca', () => {
  const m = buildLicenseRecord(generateLicenseKey(), { plan: 'monthly', email: 'a@b.com' });
  assert.ok(m.expiresAt);
  assert.ok(new Date(m.expiresAt).getTime() > Date.now() + 29 * 24 * 60 * 60 * 1000);
  assert.equal(isKnownPlan('inventado'), false);
});

test('la clave derivada de Stripe es determinista y re-emitible', () => {
  const a = deriveLicenseKey('secreto', 'cs_test_123');
  const b = deriveLicenseKey('secreto', 'cs_test_123');
  const c = deriveLicenseKey('secreto', 'cs_test_999');
  assert.equal(a, b);
  assert.notEqual(a, c);
  assert.match(a, /^IVANNA-[A-Z2-9]{4}-[A-Z2-9]{4}-[A-Z2-9]{4}$/);
});

test('MemoryStore guarda, lee y borra', async () => {
  const store = new MemoryStore();
  assert.equal(await store.get('k'), null);
  await store.set('k', '{"a":1}');
  assert.equal(await store.get('k'), '{"a":1}');
  assert.equal(await store.exists('k'), true);
  await store.del('k');
  assert.equal(await store.exists('k'), false);
});

test('firma de Stripe: válida pasa, manipulada falla, vieja falla', () => {
  const secret = 'whsec_prueba';
  const payload = JSON.stringify({ type: 'checkout.session.completed', data: { object: {} } });
  const ts = Math.floor(Date.now() / 1000);
  const sig = createHmac('sha256', secret).update(`${ts}.${payload}`, 'utf8').digest('hex');
  const header = `t=${ts},v1=${sig}`;
  const event = verifyStripeSignature(payload, header, secret);
  assert.equal(event.type, 'checkout.session.completed');

  assert.throws(() => verifyStripeSignature(payload + 'x', header, secret), /invalid_signature/);
  const oldTs = ts - 1000;
  const oldSig = createHmac('sha256', secret).update(`${oldTs}.${payload}`, 'utf8').digest('hex');
  assert.throws(() => verifyStripeSignature(payload, `t=${oldTs},v1=${oldSig}`, secret), /stale/);
  assert.throws(() => verifyStripeSignature(payload, 't=abc', secret), /invalid_signature/);
});

test('checkoutDetails lee email y plan de metadata', () => {
  const event = {
    type: 'checkout.session.completed',
    data: {
      object: {
        id: 'cs_test_1',
        customer_details: { email: 'comprador@ejemplo.com' },
        metadata: { plan: 'monthly' },
      },
    },
  };
  assert.deepEqual(checkoutDetails(event), {
    email: 'comprador@ejemplo.com',
    plan: 'monthly',
    sessionId: 'cs_test_1',
  });
  const sinMeta = { data: { object: { id: 'cs_x', customer_email: 'otro@ejemplo.com' } } };
  assert.equal(checkoutDetails(sinMeta).plan, 'lifetime');
});
