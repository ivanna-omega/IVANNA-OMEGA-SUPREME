// stripe.mjs — Webhook de Stripe sin librería externa.
// Verifica la firma HMAC-SHA256 del header `Stripe-Signature` y atiende:
//   - checkout.session.completed → crea la licencia (clave determinista)
//   - invoice.payment_succeeded → renueva 30 días una suscripción mensual
//   - customer.subscription.deleted → marca la licencia como caducada
// Si Stripe no está configurado, el servidor responde 501 (ver server.mjs).
// La clave de la licencia NO se envía por email en el MVP: se deriva del
// session id (ver deriveLicenseKey) y se re-emite con /v1/licenses/reissue.
// Fase 2: envío automático por email (Resend/SendGrid).
import { createHmac, timingSafeEqual } from 'node:crypto';

const TOLERANCE_S = 300; // 5 minutos, como Stripe.

export function isStripeConfigured() {
  return Boolean(process.env.STRIPE_SECRET_KEY) && Boolean(process.env.STRIPE_WEBHOOK_SECRET);
}

// Verifica la firma de Stripe sobre el body en crudo. Devuelve el evento
// parseado o lanza Error('invalid_signature' | 'stale' | 'bad_payload').
export function verifyStripeSignature(rawBody, signatureHeader, webhookSecret) {
  if (typeof signatureHeader !== 'string') throw new Error('invalid_signature');
  const parts = Object.fromEntries(
    signatureHeader.split(',').map((p) => {
      const i = p.indexOf('=');
      return [p.slice(0, i), p.slice(i + 1)];
    }),
  );
  const ts = Number(parts.t);
  const v1s = signatureHeader.split(',').filter((p) => p.startsWith('v1=')).map((p) => p.slice(3));
  if (!ts || v1s.length === 0) throw new Error('invalid_signature');
  if (Math.abs(Date.now() / 1000 - ts) > TOLERANCE_S) throw new Error('stale');
  const signed = `${ts}.${rawBody}`;
  const expected = createHmac('sha256', webhookSecret).update(signed, 'utf8').digest('hex');
  const ok = v1s.some((v1) => {
    const a = Buffer.from(v1, 'utf8');
    const b = Buffer.from(expected, 'utf8');
    return a.length === b.length && timingSafeEqual(a, b);
  });
  if (!ok) throw new Error('invalid_signature');
  let event;
  try {
    event = JSON.parse(rawBody);
  } catch {
    throw new Error('bad_payload');
  }
  return event;
}

// Extrae (email, plan) de un Checkout Session. El plan se lee de
// metadata.plan; si no viene, se infiere del primer price id conocido.
// `priceMap`: { priceId: plan } configurado por env STRIPE_PRICE_MAP (JSON).
export function checkoutDetails(event, priceMap = {}) {
  const session = event?.data?.object || {};
  const email =
    session.customer_details?.email || session.customer_email || null;
  const plan =
    session.metadata?.plan ||
    (session.line_items?.data?.[0]?.price?.id
      ? priceMap[session.line_items.data[0].price.id]
      : null) ||
    'lifetime';
  return { email, plan, sessionId: session.id || null };
}

// Meses a extender por cada pago de suscripción mensual.
export const RENEWAL_MONTHS = 1;
