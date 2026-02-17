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

// Parse multipart/form-data for file upload (minimal implementation)
function parseMultipart(body: Buffer, boundary: string): { filename: string; data: Buffer } | null {
  const boundaryBuf = Buffer.from('--' + boundary);
  const parts: { headers: string; data: Buffer }[] = [];

  let start = 0;
  while (start < body.length) {
    const boundaryIdx = body.indexOf(boundaryBuf, start);
    if (boundaryIdx === -1) break;
    const afterBoundary = boundaryIdx + boundaryBuf.length;
    // Check for final boundary (--)
    if (body[afterBoundary] === 45 && body[afterBoundary + 1] === 45) break;
    // Skip \r\n after boundary
    const headerStart = afterBoundary + 2;
    // Find end of headers (\r\n\r\n)
    const headerEnd = body.indexOf(Buffer.from('\r\n\r\n'), headerStart);
    if (headerEnd === -1) break;
    const headers = body.slice(headerStart, headerEnd).toString();
    const dataStart = headerEnd + 4;
    const nextBoundary = body.indexOf(boundaryBuf, dataStart);
    const dataEnd = nextBoundary === -1 ? body.length : nextBoundary - 2; // strip \r\n before boundary
    parts.push({ headers, data: body.slice(dataStart, dataEnd) });
    start = nextBoundary === -1 ? body.length : nextBoundary;
  }

  for (const part of parts) {
    const cdMatch = part.headers.match(/filename="([^"]+)"/i);
    if (cdMatch) {
      return { filename: cdMatch[1], data: part.data };
    }
  }
  return null;
}

const server = http.createServer((req, res) => {
  const url = req.url ?? '/';
  const method = req.method ?? 'GET';

  // REST: list scenarios
  if ((url === '/api/scenarios' || url.startsWith('/api/scenarios?')) && method === 'GET') {
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

  // REST: upload scenario
  if (url === '/api/scenarios/upload' && method === 'POST') {
    const chunks: Buffer[] = [];
    req.on('data', (chunk: Buffer) => chunks.push(chunk));
    req.on('end', () => {
      const body = Buffer.concat(chunks);
      const contentType = req.headers['content-type'] ?? '';

      let jsonContent: string | null = null;
      let scenarioName = `uploaded-${Date.now()}`;

      if (contentType.includes('multipart/form-data')) {
        const boundaryMatch = contentType.match(/boundary=([^\s;]+)/);
        if (!boundaryMatch) {
          res.writeHead(400, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: 'Missing multipart boundary' }));
          return;
        }
        const parsed = parseMultipart(body, boundaryMatch[1]);
        if (!parsed) {
          res.writeHead(400, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: 'Could not parse multipart body' }));
          return;
        }
        jsonContent = parsed.data.toString('utf8');
        // Use the uploaded filename (strip .json if present) as scenario name
        scenarioName = parsed.filename.replace(/\.json$/i, '');
      } else if (contentType.includes('application/json')) {
        jsonContent = body.toString('utf8');
      } else {
        // Treat raw body as JSON
        jsonContent = body.toString('utf8');
      }

      // Validate JSON
      let parsed: unknown;
      try {
        parsed = JSON.parse(jsonContent);
      } catch {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: 'Invalid JSON' }));
        return;
      }

      // Extract name from JSON if available
      if (parsed && typeof parsed === 'object' && 'name' in parsed) {
        const nameVal = (parsed as Record<string, unknown>).name;
        if (typeof nameVal === 'string' && nameVal.trim()) {
          scenarioName = nameVal.trim().replace(/[^a-zA-Z0-9_-]/g, '-');
        }
      }

      // Sanitize filename
      scenarioName = scenarioName.replace(/[^a-zA-Z0-9_-]/g, '-').slice(0, 64);
      const destPath = path.join(SCENARIOS_DIR, `${scenarioName}.json`);

      try {
        fs.writeFileSync(destPath, JSON.stringify(parsed, null, 2));
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ name: scenarioName }));
      } catch (err) {
        res.writeHead(500, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ error: String(err) }));
      }
    });
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

  // ── Playback state ──────────────────────────────────────────────────
  interface BufferedEvent { raw: string; timestamp_us: number }

  let eventBuffer: BufferedEvent[] = [];
  let playbackIndex   = 0;
  let playing         = false;
  let speedMultiplier = 1.0;
  let playbackStart   = 0;   // wall-clock ms when play started
  let simTimeAtStart  = 0;   // sim timestamp_us at play start
  let playbackTimer: ReturnType<typeof setInterval> | null = null;

  function stopPlaybackTimer() {
    if (playbackTimer) {
      clearInterval(playbackTimer);
      playbackTimer = null;
    }
  }

  function drainEvents() {
    if (!playing) return;
    const wallNow = Date.now();
    const wallElapsed = wallNow - playbackStart; // ms
    // How many sim-microseconds should have elapsed?
    const simElapsed_us = wallElapsed * speedMultiplier * 1000; // ms → us
    const simNow_us = simTimeAtStart + simElapsed_us;

    while (playbackIndex < eventBuffer.length) {
      const evt = eventBuffer[playbackIndex];
      if (evt.timestamp_us <= simNow_us) {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(evt.raw);
        }
        playbackIndex++;
      } else {
        break;
      }
    }

    // If we've sent everything, stop
    if (playbackIndex >= eventBuffer.length && eventBuffer.length > 0) {
      stopPlaybackTimer();
      playing = false;
    }
  }

  function startPlayback(fromIndex = playbackIndex) {
    stopPlaybackTimer();
    playbackIndex  = fromIndex;
    if (fromIndex < eventBuffer.length) {
      simTimeAtStart = eventBuffer[fromIndex].timestamp_us;
    } else {
      simTimeAtStart = 0;
    }
    playbackStart  = Date.now();
    playing        = true;
    playbackTimer  = setInterval(drainEvents, 50);  // 20 Hz tick
    drainEvents();
  }

  function pausePlayback() {
    stopPlaybackTimer();
    playing = false;
  }

  function restartPlayback() {
    // Re-send sim_reset first
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'sim_reset', timestamp_us: 0 }));
    }
    startPlayback(0);
  }

  // ── Simulator process ────────────────────────────────────────────────

  function startSimulator(scenarioPath: string) {
    stopPlaybackTimer();
    if (sim) {
      console.log('[relay] Killing previous simulator');
      sim.kill('SIGTERM');
      sim = null;
    }

    // Reset buffer and state
    eventBuffer    = [];
    playbackIndex  = 0;
    playing        = false;

    // Tell UI to reset state before new sim events arrive
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'sim_reset', timestamp_us: 0 }));
    }
    console.log(`[relay] Spawning simulator: ${ENGINE_BIN} ${scenarioPath}`);

    sim = spawn(ENGINE_BIN, [scenarioPath], { stdio: ['ignore', 'pipe', 'pipe'] });

    // Buffer all events from C engine stdout
    let lineBuffer = '';
    sim.stdout?.on('data', (chunk: Buffer) => {
      lineBuffer += chunk.toString();
      const lines = lineBuffer.split('\n');
      lineBuffer = lines.pop() ?? '';
      for (const line of lines) {
        const trimmed = line.trim();
        if (trimmed.startsWith('{')) {
          try {
            const parsed = JSON.parse(trimmed) as { type: string; timestamp_us?: number };
            const ts = typeof parsed.timestamp_us === 'number' ? parsed.timestamp_us : 0;

            // sim_reset is sent immediately (not buffered)
            if (parsed.type === 'sim_reset') {
              if (ws.readyState === WebSocket.OPEN) ws.send(trimmed);
            } else {
              eventBuffer.push({ raw: trimmed, timestamp_us: ts });
            }
          } catch {
            // Ignore malformed lines
          }
        }
      }
    });

    sim.stderr?.on('data', (chunk: Buffer) => {
      process.stderr.write(`[sim] ${chunk.toString()}`);
    });

    sim.on('exit', (code, signal) => {
      console.log(`[relay] Simulator exited (code=${code}, signal=${signal})`);
      // Flush remaining buffer line
      if (lineBuffer.trim().startsWith('{')) {
        try {
          const parsed = JSON.parse(lineBuffer.trim()) as { timestamp_us?: number };
          const ts = typeof parsed.timestamp_us === 'number' ? parsed.timestamp_us : 0;
          eventBuffer.push({ raw: lineBuffer.trim(), timestamp_us: ts });
        } catch { /* ignore */ }
      }

      sim = null;

      // Ready to play — wait for user to press play
      console.log(`[relay] Buffered ${eventBuffer.length} events — ready (paused)`);
      playbackIndex = 0;
      if (eventBuffer.length > 0) {
        simTimeAtStart = eventBuffer[0].timestamp_us;
      }
      // Send initial events (sim_reset + node_joined + config) so the UI shows the topology
      for (let i = 0; i < eventBuffer.length; i++) {
        const evt = JSON.parse(eventBuffer[i].raw);
        if (evt.type === 'node_joined' || evt.type === 'config' || evt.type === 'sim_reset') {
          if (ws.readyState === WebSocket.OPEN) ws.send(eventBuffer[i].raw);
        } else {
          playbackIndex = i;
          break;
        }
      }
      // Notify UI that sim is loaded but paused
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'sim_ready', total_events: eventBuffer.length, timestamp_us: 0 }));
      }

      // Append sim_ended at the end of the buffer
      const endMsg = JSON.stringify({ type: 'sim_ended', code, signal, timestamp_us: 
        eventBuffer.length > 0 ? eventBuffer[eventBuffer.length - 1].timestamp_us + 1 : 0 });
      eventBuffer.push({ raw: endMsg, timestamp_us: 
        eventBuffer.length > 0 ? eventBuffer[eventBuffer.length - 1].timestamp_us + 1 : 0 });
    });
  }

  // Handle messages from client
  ws.on('message', (data: Buffer) => {
    try {
      const msg = JSON.parse(data.toString()) as { type: string; scenario?: string; value?: number };

      if (msg.type === 'start') {
        const scenarioName = msg.scenario;
        const scenarioPath = scenarioName
          ? path.resolve(SCENARIOS_DIR, `${scenarioName}.json`)
          : DEFAULT_SCENARIO;

        if (!fs.existsSync(scenarioPath)) {
          ws.send(JSON.stringify({ type: 'error', message: `Scenario not found: ${scenarioPath}` }));
          return;
        }
        startSimulator(scenarioPath);
        return;
      }

      // Playback control commands
      if (msg.type === 'play') {
        startPlayback();
        return;
      }
      if (msg.type === 'pause') {
        pausePlayback();
        return;
      }
      if (msg.type === 'restart') {
        restartPlayback();
        return;
      }
      if (msg.type === 'speed' && typeof msg.value === 'number') {
        const newSpeed = Math.max(0.1, Math.min(200, msg.value));
        if (playing) {
          // Re-anchor the playback start so speed change is seamless
          const wallNow = Date.now();
          const wallElapsed = wallNow - playbackStart;
          const simElapsed_us = wallElapsed * speedMultiplier * 1000;
          simTimeAtStart = simTimeAtStart + simElapsed_us;
          playbackStart  = wallNow;
        }
        speedMultiplier = newSpeed;
        return;
      }
    } catch {
      // Ignore invalid messages
    }
  });

  // Auto-start with default scenario within 500ms if client doesn't send start
  const autoStart = setTimeout(() => {
    if (!sim && eventBuffer.length === 0) {
      startSimulator(DEFAULT_SCENARIO);
    }
  }, 500);

  ws.on('close', () => {
    console.log('[relay] Client disconnected');
    clearTimeout(autoStart);
    stopPlaybackTimer();
    if (sim) {
      sim.kill('SIGTERM');
      sim = null;
    }
  });

  ws.on('error', (err) => {
    console.error('[relay] WebSocket error:', err);
    clearTimeout(autoStart);
    stopPlaybackTimer();
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
