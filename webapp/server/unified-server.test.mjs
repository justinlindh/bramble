// @vitest-environment node

import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { mkdtemp, mkdir, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';

let server;
let base;
let otaUpstream;
let otaBase;
let staticServer;
let staticBase;
let staticDistDir;

beforeAll(async () => {
  otaUpstream = new ResponseServer();
  await otaUpstream.start();
  otaBase = otaUpstream.baseUrl;

  const { createUnifiedServer } = await import('./unified-server.mjs');
  server = createUnifiedServer({ otaBaseUrl: otaBase });

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
  if (otaUpstream) {
    await otaUpstream.stop();
  }
  if (staticServer) {
    await new Promise((resolve, reject) => {
      staticServer.close((err) => (err ? reject(err) : resolve()));
    });
  }
  if (staticDistDir) {
    await rm(staticDistDir, { recursive: true, force: true });
  }
});

class ResponseServer {
  constructor() {
    this.server = null;
    this.baseUrl = '';
  }

  async start() {
    const { createServer } = await import('node:http');
    this.server = createServer((req, res) => {
      if (req.url === '/ota/index.json') {
        res.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' });
        res.end(JSON.stringify({ releases: [{ version: 'v1.2.3', channel: 'stable', artifacts: [] }] }));
        return;
      }

      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('missing');
    });

    await new Promise((resolve) => {
      this.server.listen(0, '127.0.0.1', resolve);
    });

    const address = this.server.address();
    this.baseUrl = `http://127.0.0.1:${address.port}`;
  }

  async stop() {
    if (!this.server) return;
    await new Promise((resolve, reject) => {
      this.server.close((err) => (err ? reject(err) : resolve()));
    });
  }
}

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
    const res = await fetch(`${base}/ws-proxy?target=192.168.1.20`);
    expect(res.status).toBe(403);
    expect(await res.json()).toEqual({ error: 'ws proxy disabled in hosted mode' });
  });

  it('proxies /ota/index.json to OTA upstream', async () => {
    const res = await fetch(`${base}/ota/index.json`);
    expect(res.status).toBe(200);
    expect(res.headers.get('content-type')).toContain('application/json');
    expect(await res.json()).toEqual({
      releases: [{ version: 'v1.2.3', channel: 'stable', artifacts: [] }],
    });
  });

  it('passes through OTA upstream status codes', async () => {
    const res = await fetch(`${base}/ota/not-found.json`);
    expect(res.status).toBe(404);
    expect(await res.text()).toBe('missing');
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
    const res = await fetch(`${localBase}/ws-proxy?target=192.168.1.20`);
    expect(res.status).toBe(426);
    expect(await res.json()).toEqual({ error: 'websocket upgrade required' });
  });
});

describe('web flasher static asset caching', () => {
  beforeAll(async () => {
    const { createUnifiedServer } = await import('./unified-server.mjs');
    staticDistDir = await mkdtemp(path.join(os.tmpdir(), 'bramble-static-'));
    await mkdir(path.join(staticDistDir, 'web-flasher'), { recursive: true });
    await writeFile(path.join(staticDistDir, 'index.html'), '<html><body>ok</body></html>');
    await writeFile(path.join(staticDistDir, 'web-flasher', 'flasher.js'), 'console.log("ok");');

    staticServer = createUnifiedServer({ distDir: staticDistDir, otaBaseUrl: otaBase });
    await new Promise((resolve) => {
      staticServer.listen(0, '127.0.0.1', resolve);
    });
    const address = staticServer.address();
    staticBase = `http://127.0.0.1:${address.port}`;
  });

  it('sets no-store cache-control on /web-flasher assets', async () => {
    const res = await fetch(`${staticBase}/web-flasher/flasher.js`);
    expect(res.status).toBe(200);
    expect(res.headers.get('cache-control')).toContain('no-store');
  });
});
