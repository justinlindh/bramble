// @vitest-environment node

import { afterAll, beforeAll, describe, expect, it } from 'vitest';

let server;
let base;

beforeAll(async () => {
  const { createUnifiedServer } = await import('./unified-server.mjs');
  server = createUnifiedServer();

  await new Promise((resolve) => {
    server.listen(0, '127.0.0.1', resolve);
  });

  const address = server.address();
  base = `http://127.0.0.1:${address.port}`;
});

afterAll(async () => {
  if (!server) return;
  await new Promise((resolve, reject) => {
    server.close((err) => (err ? reject(err) : resolve()));
  });
});

describe('unified runtime api', () => {
  it('returns mode from /api/mode', async () => {
    const res = await fetch(`${base}/api/mode`);
    expect(res.status).toBe(200);
    expect(await res.json()).toEqual({ mode: 'hosted' });
  });

  it('returns proxyEnabled=false in hosted mode', async () => {
    const res = await fetch(`${base}/api/capabilities`);
    expect(res.status).toBe(200);

    const json = await res.json();
    expect(json.mode).toBe('hosted');
    expect(json.proxyEnabled).toBe(false);
  });

  it('returns ok=true from /api/healthz', async () => {
    const res = await fetch(`${base}/api/healthz`);
    expect(res.status).toBe(200);
    expect(await res.json()).toEqual({ ok: true });
  });

  it('returns 405 for unsupported method on API routes', async () => {
    const res = await fetch(`${base}/api/mode`, { method: 'POST' });
    expect(res.status).toBe(405);
  });

  it('returns 400 for malformed escaped path', async () => {
    const res = await fetch(`${base}/%E0%A4%A`);
    expect(res.status).toBe(400);
  });

  it('returns 403 for /ws-proxy in hosted mode', async () => {
    const res = await fetch(`${base}/ws-proxy?target=192.0.2.0`);
    expect(res.status).toBe(403);
    expect(await res.json()).toEqual({ error: 'ws proxy disabled in hosted mode' });
  });
});

describe('ws proxy behavior in local mode', () => {
  let localServer;
  let localBase;

  beforeAll(async () => {
    const { createUnifiedServer } = await import('./unified-server.mjs');
    localServer = createUnifiedServer({ mode: 'local' });

    await new Promise((resolve) => {
      localServer.listen(0, '127.0.0.1', resolve);
    });

    const address = localServer.address();
    localBase = `http://127.0.0.1:${address.port}`;
  });

  afterAll(async () => {
    if (!localServer) return;
    await new Promise((resolve, reject) => {
      localServer.close((err) => (err ? reject(err) : resolve()));
    });
  });

  it('returns 403 for invalid target in local mode', async () => {
    const res = await fetch(`${localBase}/ws-proxy?target=8.8.8.8`);
    expect(res.status).toBe(403);
    expect(await res.json()).toEqual({ error: 'target is not allowed by local policy' });
  });

  it('returns 426 for valid target without websocket upgrade', async () => {
    const res = await fetch(`${localBase}/ws-proxy?target=192.0.2.0`);
    expect(res.status).toBe(426);
    expect(await res.json()).toEqual({ error: 'websocket upgrade required' });
  });
});
