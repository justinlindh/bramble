import http from 'http';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { spawn, ChildProcess } from 'child_process';
import { WebSocketServer, WebSocket } from 'ws';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const UI_DIST = path.resolve(__dirname, '../ui/dist');
const ENGINE_BIN = path.resolve(__dirname, '../engine/bramble-sim');
const SCENARIOS_DIR = path.resolve(__dirname, '../scenarios');
const DEFAULT_SCENARIO = path.resolve(SCENARIOS_DIR, '10-node-grid.json');
const PORT = parseInt(process.env.PORT ?? '3000', 10);

// MIME types for static file serving
const MIME: Record<string, string> = {
  '.html': 'text/html',
  '.js':   'application/javascript',
  '.css':  'text/css',
  '.json': 'application/json',
  '.svg':  'image/svg+xml',
  '.ico':  'image/x-icon',
  '.png':  'image/png',
  '.woff2': 'font/woff2',
};

function serveStatic(req: http.IncomingMessage, res: http.ServerResponse): boolean {
  if (!req.url) return false;
  let urlPath = req.url.split('?')[0];
  if (urlPath === '/') urlPath = '/index.html';

  const filePath = path.join(UI_DIST, urlPath);
  // Security: don't serve files outside UI_DIST
  if (!filePath.startsWith(UI_DIST)) return false;

  if (fs.existsSync(filePath) && fs.statSync(filePath).isFile()) {
    const ext = path.extname(filePath);
    const mime = MIME[ext] ?? 'application/octet-stream';
    const content = fs.readFileSync(filePath);
    res.writeHead(200, { 'Content-Type': mime, 'Content-Length': content.length });
    res.end(content);
    return true;
  }

  // SPA fallback: serve index.html for any unmatched route
  const indexPath = path.join(UI_DIST, 'index.html');
  if (fs.existsSync(indexPath)) {
    const content = fs.readFileSync(indexPath);
    res.writeHead(200, { 'Content-Type': 'text/html', 'Content-Length': content.length });
    res.end(content);
    return true;
  }

  return false;
}

const server = http.createServer((req, res) => {
  const url = req.url ?? '/';

  // REST: list scenarios
  if (url === '/api/scenarios' || url.startsWith('/api/scenarios?')) {
    try {
      const files = fs.readdirSync(SCENARIOS_DIR)
        .filter(f => f.endsWith('.json'))
        .map(f => f.replace('.json', ''));
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify(files));
    } catch (err) {
      res.writeHead(500, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: String(err) }));
    }
    return;
  }

  // Static files
  if (!serveStatic(req, res)) {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end('Not Found');
  }
});

const wss = new WebSocketServer({ server });

wss.on('connection', (ws: WebSocket) => {
  console.log('[relay] Client connected');

  let sim: ChildProcess | null = null;

  function startSimulator(scenarioPath: string) {
    // Kill any running simulation first
    if (sim) {
      console.log('[relay] Killing previous simulator');
      sim.kill('SIGTERM');
      sim = null;
    }

    // Tell UI to reset state before new sim events arrive
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'sim_reset', timestamp_us: 0 }));
    }
    console.log(`[relay] Spawning simulator: ${ENGINE_BIN} ${scenarioPath}`);

    sim = spawn(ENGINE_BIN, [scenarioPath], { stdio: ['ignore', 'pipe', 'pipe'] });

    // Pipe stdout JSON lines to WebSocket
    let buffer = '';
    sim.stdout?.on('data', (chunk: Buffer) => {
      buffer += chunk.toString();
      const lines = buffer.split('\n');
      buffer = lines.pop() ?? '';
      for (const line of lines) {
        const trimmed = line.trim();
        if (trimmed.startsWith('{')) {
          if (ws.readyState === WebSocket.OPEN) {
            ws.send(trimmed);
          }
        }
      }
    });

    // Pipe stderr to console
    sim.stderr?.on('data', (chunk: Buffer) => {
      process.stderr.write(`[sim] ${chunk.toString()}`);
    });

    sim.on('exit', (code, signal) => {
      console.log(`[relay] Simulator exited (code=${code}, signal=${signal})`);
      // Flush remaining buffer
      if (buffer.trim().startsWith('{') && ws.readyState === WebSocket.OPEN) {
        ws.send(buffer.trim());
      }
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'sim_ended', code, signal }));
        // Keep connection open so UI shows "Completed" instead of "Disconnected"
      }
      sim = null;
    });
  }

  // Handle messages from client (e.g., { type: "start", scenario: "test-2-node" })
  ws.on('message', (data: Buffer) => {
    try {
      const msg = JSON.parse(data.toString());
      if (msg.type === 'start') {
        const scenarioName = msg.scenario as string | undefined;
        const scenarioPath = scenarioName
          ? path.resolve(SCENARIOS_DIR, `${scenarioName}.json`)
          : DEFAULT_SCENARIO;

        if (!fs.existsSync(scenarioPath)) {
          ws.send(JSON.stringify({ type: 'error', message: `Scenario not found: ${scenarioPath}` }));
          return;
        }
        startSimulator(scenarioPath);
      }
    } catch {
      // Ignore invalid messages
    }
  });

  // Auto-start with default scenario if client doesn't send a start message within 500ms
  let autoStartFired = false;
  const autoStart = setTimeout(() => {
    if (!sim) {
      autoStartFired = true;
      startSimulator(DEFAULT_SCENARIO);
    }
  }, 500);

  ws.on('close', () => {
    console.log('[relay] Client disconnected');
    clearTimeout(autoStart);
    if (sim) {
      sim.kill('SIGTERM');
      sim = null;
    }
  });

  ws.on('error', (err) => {
    console.error('[relay] WebSocket error:', err);
    clearTimeout(autoStart);
    if (sim) {
      sim.kill('SIGTERM');
      sim = null;
    }
  });
});

server.listen(PORT, '0.0.0.0', () => {
  console.log(`[relay] Listening on http://localhost:${PORT}`);
  console.log(`[relay] Serving UI from ${UI_DIST}`);
  console.log(`[relay] Engine: ${ENGINE_BIN}`);
});
