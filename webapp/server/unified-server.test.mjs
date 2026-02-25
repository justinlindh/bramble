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
});
