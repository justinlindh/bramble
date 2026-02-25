import { createServer } from 'node:http';
import { createReadStream, existsSync } from 'node:fs';
import { stat } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const DEFAULT_DIST_DIR = path.resolve(__dirname, '..', 'dist');

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

export function createUnifiedServer({ mode = resolveMode(), distDir = DEFAULT_DIST_DIR } = {}) {
  mode = resolveMode(mode);

  return createServer(async (req, res) => {
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

    if (req.method !== 'GET' && req.method !== 'HEAD') {
      res.writeHead(405, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('Method not allowed');
      return;
    }

    let requestPath = decodeURIComponent(url.pathname);
    if (requestPath === '/') requestPath = '/index.html';

    const safePath = path.normalize(requestPath).replace(/^([.][./\\])+/, '');
    const filePath = path.resolve(distDir, `.${safePath}`);

    if (!filePath.startsWith(path.resolve(distDir))) {
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

    res.writeHead(200, { 'Content-Type': contentType(candidatePath) });
    if (req.method === 'HEAD') {
      res.end();
      return;
    }

    createReadStream(candidatePath).pipe(res);
  });
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
