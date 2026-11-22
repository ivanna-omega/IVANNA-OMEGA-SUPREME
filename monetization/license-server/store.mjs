// store.mjs — Almacén de licencias detrás de una sola interfaz.
//   get(key) -> string|null · set(key, value) -> void
//   del(key) -> void · exists(key) -> boolean
// Dos implementaciones:
//   - MemoryStore: en memoria, solo para desarrollo/pruebas. Se pierde al
//     reiniciar. NUNCA usarla en producción (las licencias desaparecerían).
//   - RedisStore: Render Key Value (compatible con Redis). Es el store de
//     producción elegido (ver README: Postgres free caduca a los 90 días;
//     el lookup de licencias es clave-valor puro, sin relaciones).
// Se elige con REDIS_URL: si está puesta, Redis; si no, memoria con aviso.
import net from 'node:net';

export class MemoryStore {
  constructor() {
    this.map = new Map();
  }
  async get(key) {
    const v = this.map.get(key);
    return v === undefined ? null : v;
  }
  async set(key, value) {
    this.map.set(key, value);
  }
  async del(key) {
    this.map.delete(key);
  }
  async exists(key) {
    return this.map.has(key);
  }
}

// Cliente RESP mínimo (sin dependencias): solo GET/SET/DEL/EXISTS.
// Suficiente para el store de licencias; no es un cliente Redis general.
export class RedisStore {
  constructor(url) {
    this.url = new URL(url);
    this.host = this.url.hostname;
    this.port = Number(this.url.port || 6379);
    this.password = this.url.password ? decodeURIComponent(this.url.password) : null;
    this.socket = null;
    this.buffer = Buffer.alloc(0);
    this.pending = []; // cola de {resolve, reject}
    this.authed = false;
  }

  connect() {
    if (this.socket && !this.socket.destroyed) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const socket = net.createConnection({ host: this.host, port: this.port });
      const onError = (err) => { this.socket = null; reject(err); };
      socket.once('error', onError);
      socket.once('connect', async () => {
        socket.removeListener('error', onError);
        socket.on('error', () => { this.socket = null; });
        socket.on('data', (chunk) => this._onData(chunk));
        socket.on('close', () => { this.socket = null; });
        this.socket = socket;
        try {
          if (this.password) await this._command('AUTH', this.password);
          this.authed = true;
          resolve();
        } catch (err) {
          reject(err);
        }
      });
    });
  }

  _onData(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    for (;;) {
      const parsed = this._tryParse();
      if (!parsed) break;
      const job = this.pending.shift();
      if (!job) continue;
      if (parsed instanceof Error) job.reject(parsed);
      else job.resolve(parsed);
    }
  }

  // Parsea UNA respuesta RESP del buffer. null si aún no está completa.
  _tryParse() {
    const buf = this.buffer;
    if (buf.length === 0) return null;
    const type = String.fromCharCode(buf[0]);
    const eol = buf.indexOf('\r\n');
    if (eol === -1) return null;
    const line = buf.subarray(1, eol).toString('utf8');
    if (type === '+') {
      this.buffer = buf.subarray(eol + 2);
      return line;
    }
    if (type === '-') {
      this.buffer = buf.subarray(eol + 2);
      return new Error(`redis: ${line}`);
    }
    if (type === ':') {
      this.buffer = buf.subarray(eol + 2);
      return Number(line);
    }
    if (type === '$') {
      const len = Number(line);
      if (len === -1) {
        this.buffer = buf.subarray(eol + 2);
        return null;
      }
      if (buf.length < eol + 2 + len + 2) return null;
      const value = buf.subarray(eol + 2, eol + 2 + len).toString('utf8');
      this.buffer = buf.subarray(eol + 2 + len + 2);
      return value;
    }
    // No soportamos arrays aquí; el store no los usa.
    this.buffer = buf.subarray(eol + 2);
    return new Error(`redis: tipo RESP no soportado: ${type}`);
  }

  _command(...args) {
    return new Promise((resolve, reject) => {
      const parts = [`*${args.length}\r\n`];
      for (const a of args) {
        const s = String(a);
        parts.push(`$${Buffer.byteLength(s)}\r\n${s}\r\n`);
      }
      this.pending.push({ resolve, reject });
      this.socket.write(parts.join(''), (err) => {
        if (err) {
          const job = this.pending.pop();
          if (job) job.reject(err);
        }
      });
    });
  }

  async _run(...args) {
    try {
      await this.connect();
      return await this._command(...args);
    } catch (err) {
      this.socket = null;
      throw err;
    }
  }

  async get(key) {
    return this._run('GET', key);
  }
  async set(key, value) {
    await this._run('SET', key, value);
  }
  async del(key) {
    await this._run('DEL', key);
  }
  async exists(key) {
    return (await this._run('EXISTS', key)) === 1;
  }
}

// Prefijo de claves: separa las licencias de cualquier otra cosa en el KV.
export const licenseKey = (keyHash) => `license:${keyHash}`;
export const emailIndexKey = (emailHash) => `email:${emailHash}`;

export function createStore() {
  if (process.env.REDIS_URL) {
    console.log('[store] usando RedisStore (Render Key Value)');
    return new RedisStore(process.env.REDIS_URL);
  }
  console.warn('[store] AVISO: usando MemoryStore (en memoria). Las licencias se pierden al reiniciar. Pon REDIS_URL en producción.');
  return new MemoryStore();
}
