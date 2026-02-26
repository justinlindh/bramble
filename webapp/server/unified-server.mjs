import { createServer } from 'node:http';
import { createReadStream, existsSync } from 'node:fs';
import { stat } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { WebSocket, WebSocketServer } from 'ws';
import { isAllowedTarget, parseAllowlist, splitTarget } from './target-policy.mjs';
import { handleConnection as handleMockConnection } from '../mock/handler.mjs';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const DEFAULT_DIST_DIR = path.resolve(__dirname, '..', 'dist');
const DEFAULT_OTA_BASE_URL = process.env.OTA_BASE_URL || 'https://bramblemesh.org';
const WS_CONNECT_TIMEOUT_MS = 10_000;

function json(res, status, body) {
  res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
  res.end(JSON.stringify(body));
}

function getCapabilities(mode) {
  const isLocal = mode === 'local';

  return {
    mode,
    proxyEnabled: isLocal,
    localLanAllowed: isLocal,
    // Runtime/browser feature detection is handled client-side in later tasks.
    // For now these are policy defaults from server mode only.
    usbEnabled: true,
    bleEnabled: true,
  };
}

function contentType(filePath) {
  if (filePath.endsWith('.html')) return 'text/html; charset=utf-8';
  if (filePath.endsWith('.js')) return 'application/javascript; charset=utf-8';
  if (filePath.endsWith('.css')) return 'text/css; charset=utf-8';
  if (filePath.endsWith('.json')) return 'application/json; charset=utf-8';
  if (filePath.endsWith('.svg')) return 'image/svg+xml';
  if (filePath.endsWith('.png')) return 'image/png';
  if (filePath.endsWith('.ico')) return 'image/x-icon';
  return 'application/octet-stream';
}

function resolveMode(inputMode = process.env.MODE) {
  return inputMode === 'local' ? 'local' : 'hosted';
}

function isTargetAllowed(rawTarget, { mode, allowlist }) {
  const parsed = splitTarget(rawTarget);
  if (!parsed) return { ok: false, reason: 'target must be a literal IPv4 address' };

  if (!isAllowedTarget(parsed.host, { mode, allowlist })) {
    return { ok: false, reason: 'target is not allowed by local policy' };
  }

  return { ok: true, parsed };
}

function writeUpgradeError(socket, statusCode, body) {
  socket.write(
    `HTTP/1.1 ${statusCode} ${statusCode === 403 ? 'Forbidden' : 'Bad Request'}\r\n` +
      'Content-Type: application/json; charset=utf-8\r\n' +
      'Connection: close\r\n' +
      '\r\n' +
      JSON.stringify(body),
  );
  socket.destroy();
}

function validateProxyRequest(url, { mode, allowlist }) {
  if (mode !== 'local') {
    return { ok: false, status: 403, error: 'ws proxy disabled in hosted mode' };
  }

  const check = isTargetAllowed(url.searchParams.get('target'), { mode, allowlist });
  if (!check.ok) {
    return { ok: false, status: 403, error: check.reason };
  }

  return { ok: true, parsed: check.parsed };
}

export function createUnifiedServer({
  mode = resolveMode(),
  distDir = DEFAULT_DIST_DIR,
  allowlist = parseAllowlist(),
  otaBaseUrl = DEFAULT_OTA_BASE_URL,
} = {}) {
  mode = resolveMode(mode);
  otaBaseUrl = String(otaBaseUrl).replace(/\/+$/, '');

  const server = createServer(async (req, res) => {
    const url = new URL(req.url || '/', 'http://localhost');

    if (req.method === 'GET' && url.pathname === '/api/healthz') {
      json(res, 200, { ok: true });
      return;
    }

    if (req.method === 'GET' && url.pathname === '/api/mode') {
      json(res, 200, { mode });
      return;
    }

    if (req.method === 'GET' && url.pathname === '/api/capabilities') {
      json(res, 200, getCapabilities(mode));
      return;
    }

    if (req.method === 'GET' && url.pathname === '/ws-proxy') {
      const check = validateProxyRequest(url, { mode, allowlist });
      if (!check.ok) {
        json(res, check.status, { error: check.error });
        return;
      }

      json(res, 426, { error: 'websocket upgrade required' });
      return;
    }

    if ((req.method === 'GET' || req.method === 'HEAD') && url.pathname.startsWith('/ota/')) {
      const upstreamUrl = `${otaBaseUrl}${url.pathname}${url.search}`;
      try {
        const upstreamRes = await fetch(upstreamUrl, {
          method: req.method,
          headers: { Accept: req.headers.accept || '*/*' },
          redirect: 'follow',
        });

        const outHeaders = { 'Cache-Control': 'no-store' };
        const contentType = upstreamRes.headers.get('content-type');
        const contentLength = upstreamRes.headers.get('content-length');
        const etag = upstreamRes.headers.get('etag');
        const lastModified = upstreamRes.headers.get('last-modified');

        if (contentType) outHeaders['Content-Type'] = contentType;
        if (contentLength) outHeaders['Content-Length'] = contentLength;
        if (etag) outHeaders.ETag = etag;
        if (lastModified) outHeaders['Last-Modified'] = lastModified;

        res.writeHead(upstreamRes.status, outHeaders);
        if (req.method === 'HEAD') {
          res.end();
          return;
        }

        const body = Buffer.from(await upstreamRes.arrayBuffer());
        res.end(body);
      } catch {
        json(res, 502, { error: 'ota upstream unavailable' });
      }
      return;
    }

    if (req.method !== 'GET' && req.method !== 'HEAD') {
      res.writeHead(405, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('Method not allowed');
      return;
    }

    let requestPath;
    try {
      requestPath = decodeURIComponent(url.pathname);
    } catch {
      res.writeHead(400, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('Bad request');
      return;
    }

    if (requestPath === '/') requestPath = '/index.html';

    const safePath = path.normalize(requestPath).replace(/^([.][./\\])+/, '');
    const distRoot = path.resolve(distDir);
    const filePath = path.resolve(distRoot, `.${safePath}`);
    const relativeToRoot = path.relative(distRoot, filePath);

    if (relativeToRoot.startsWith('..') || path.isAbsolute(relativeToRoot)) {
      res.writeHead(403, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('Forbidden');
      return;
    }

    let candidatePath = filePath;
    try {
      const fileStat = await stat(candidatePath);
      if (fileStat.isDirectory()) {
        candidatePath = path.join(candidatePath, 'index.html');
      }
    } catch {
      candidatePath = path.resolve(distDir, 'index.html');
    }

    if (!existsSync(candidatePath)) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('Not found');
      return;
    }

    const headers = { 'Content-Type': contentType(candidatePath) };
    // HTML files (especially index.html / SPA fallback) must not be cached
    // so browsers always fetch the latest bundle references after deploys.
    if (candidatePath.endsWith('.html')) {
      headers['Cache-Control'] = 'no-cache, no-store, must-revalidate';
    }
    // Avoid stale flasher assets without manual query-string cache busting.
    if (requestPath === '/web-flasher' || requestPath.startsWith('/web-flasher/')) {
      headers['Cache-Control'] = 'no-store, max-age=0';
    }

    res.writeHead(200, headers);
    if (req.method === 'HEAD') {
      res.end();
      return;
    }

    createReadStream(candidatePath).pipe(res);
  });

  const wss = new WebSocketServer({ noServer: true });
  const mockWss = new WebSocketServer({ noServer: true });

  server.on('upgrade', (req, socket, head) => {
    let url;
    try {
      url = new URL(req.url || '/', 'http://localhost');
    } catch {
      writeUpgradeError(socket, 400, { error: 'bad request' });
      return;
    }

    if (url.pathname === '/ws') {
      mockWss.handleUpgrade(req, socket, head, (ws) => {
        handleMockConnection(ws, req);
      });
      return;
    }

    if (url.pathname !== '/ws-proxy') {
      socket.destroy();
      return;
    }

    const check = validateProxyRequest(url, { mode, allowlist });
    if (!check.ok) {
      writeUpgradeError(socket, check.status, { error: check.error });
      return;
    }

    const upstreamPath = check.parsed.port ? `${check.parsed.host}:${check.parsed.port}` : check.parsed.host;
    const upstreamUrl = `ws://${upstreamPath}/ws`;

    wss.handleUpgrade(req, socket, head, (clientWs) => {
      const upstreamWs = new WebSocket(upstreamUrl);
      let finished = false;

      const closeBoth = (code = 1011, reason = 'proxy closed') => {
        if (finished) return;
        finished = true;

        if (clientWs.readyState === WebSocket.OPEN || clientWs.readyState === WebSocket.CONNECTING) {
          clientWs.close(code, reason);
        }
        if (upstreamWs.readyState === WebSocket.OPEN || upstreamWs.readyState === WebSocket.CONNECTING) {
          upstreamWs.close(code, reason);
        }
      };

      const timeout = setTimeout(() => {
        closeBoth(1013, 'upstream timeout');
      }, WS_CONNECT_TIMEOUT_MS);

      clientWs.on('message', (message, isBinary) => {
        if (upstreamWs.readyState === WebSocket.OPEN) {
          upstreamWs.send(message, { binary: isBinary });
        }
      });

      upstreamWs.on('message', (message, isBinary) => {
        if (clientWs.readyState === WebSocket.OPEN) {
          clientWs.send(message, { binary: isBinary });
        }
      });

      upstreamWs.on('open', () => {
        clearTimeout(timeout);
      });

      upstreamWs.on('error', () => {
        clearTimeout(timeout);
        closeBoth(1011, 'upstream error');
      });

      clientWs.on('error', () => {
        clearTimeout(timeout);
        closeBoth(1011, 'client error');
      });

      upstreamWs.on('close', () => {
        clearTimeout(timeout);
        closeBoth(1000, 'upstream closed');
      });

      clientWs.on('close', () => {
        clearTimeout(timeout);
        closeBoth(1000, 'client closed');
      });
    });
  });

  return server;
}

function start() {
  const port = Number.parseInt(process.env.PORT || '8085', 10);
  const server = createUnifiedServer();

  server.listen(port, () => {
    console.log(`[unified-server] listening on :${port}`);
  });
}

if (process.argv[1] === __filename) {
  start();
}
