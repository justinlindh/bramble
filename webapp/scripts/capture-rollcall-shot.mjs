/**
 * Capture the Roll Call panel screenshot for docs/rollcall.md.
 *
 * Self-contained: builds nothing, but starts the unified server (which serves
 * the built webapp AND the embedded mock node on /ws), anchors the mock node
 * so its roll-call ledger has an authoritative expected set, drives the real
 * UI through starting a roll-call, waits for the mock fleet's answers to land,
 * and screenshots the panel itself rather than the whole page.
 *
 * Run `npm run build` first so dist/ exists.
 *
 *   PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers \
 *     node scripts/capture-rollcall-shot.mjs
 *
 * OUT defaults to ../docs/images, so a plain run refreshes the committed
 * image in place.
 */
import { spawn } from 'node:child_process';
import { existsSync, mkdirSync, readdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
import { WebSocket } from 'ws';

const here = path.dirname(fileURLToPath(import.meta.url));
const webappDir = path.resolve(here, '..');
const PORT = Number(process.env.PORT || 8186);
const OUT = process.env.OUT || path.resolve(webappDir, '..', 'docs', 'images');
const BASE = `http://127.0.0.1:${PORT}`;
// A fixed, obviously-fake anchor key: the mock validates the shape, never the
// signature, and a real key has no business in a capture script.
const ANCHOR_PUBKEY = 'a0'.repeat(32);

mkdirSync(OUT, { recursive: true });

function waitFor(fn, timeoutMs, what) {
  const deadline = Date.now() + timeoutMs;
  return new Promise((resolve, reject) => {
    const tick = async () => {
      try {
        if (await fn()) return resolve();
      } catch {
        /* keep waiting */
      }
      if (Date.now() > deadline) return reject(new Error(`timed out waiting for ${what}`));
      setTimeout(tick, 200);
    };
    tick();
  });
}

// Call a short sequence of RPCs against the embedded mock node over one
// WebSocket, resolving when the last one has answered.
function mockRpc(calls) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${PORT}/ws`);
    let i = 0;
    const send = () => ws.send(JSON.stringify({ jsonrpc: '2.0', id: i + 1, ...calls[i] }));
    ws.on('open', send);
    ws.on('message', (raw) => {
      const msg = JSON.parse(String(raw));
      if (msg.id !== i + 1) return;
      if (msg.error) {
        ws.close();
        reject(new Error(`${calls[i].method} failed: ${JSON.stringify(msg.error)}`));
        return;
      }
      i++;
      if (i < calls.length) return send();
      ws.close();
      resolve();
    });
    ws.on('error', reject);
  });
}

function resolveChromium() {
  const pinned = chromium.executablePath();
  if (existsSync(pinned)) return pinned;
  const root = process.env.PLAYWRIGHT_BROWSERS_PATH;
  if (!root) throw new Error(`chromium not found at ${pinned} and PLAYWRIGHT_BROWSERS_PATH is unset`);
  const revs = readdirSync(root)
    .filter((d) => /^chromium-\d+$/.test(d))
    .sort((a, b) => Number(a.split('-')[1]) - Number(b.split('-')[1]));
  for (const rev of revs.reverse()) {
    const exe = path.join(root, rev, 'chrome-linux', 'chrome');
    if (existsSync(exe)) {
      console.log(`using baked chromium ${rev} (playwright resolves ${path.basename(path.dirname(path.dirname(pinned)))})`);
      return exe;
    }
  }
  throw new Error(`no usable chromium under ${root}`);
}

const server = spawn('node', ['server/unified-server.mjs'], {
  cwd: webappDir,
  env: { ...process.env, PORT: String(PORT), MODE: 'local' },
  stdio: ['ignore', 'pipe', 'pipe'],
});
server.stdout.on('data', (b) => process.stdout.write(`[server] ${b}`));
server.stderr.on('data', (b) => process.stderr.write(`[server] ${b}`));

let browser;
const shutdown = async (code) => {
  if (browser) await browser.close().catch(() => {});
  server.kill('SIGTERM');
  process.exit(code);
};

try {
  await waitFor(
    async () => (await fetch(`${BASE}/`)).ok,
    20_000,
    'the unified server to serve the built app (run `npm run build` first)',
  );

  // Bring the mock node up the way an operator would before running a
  // roll-call: provision it with a network key (it boots unprovisioned and
  // INERT, exactly like real firmware) and give it a fleet anchor, so its
  // ledger has an authoritative expected set and can name the member that
  // never answers. The mock's state is per process and shared across
  // connections, so driving these two calls over its own RPC surface here is
  // equivalent to having run them from the UI.
  await mockRpc([
    { method: 'bramble.generateNetworkKey', params: {} },
    { method: 'bramble.setAnchor', params: { anchor_pubkey: ANCHOR_PUBKEY } },
  ]);

  // Browser resolution follows emulator/e2e/run_e2e.sh: use the revision this
  // playwright version resolves when it is present, otherwise the newest
  // chromium baked at PLAYWRIGHT_BROWSERS_PATH. Never `playwright install`:
  // this box has no CDN access and a job-time download would hide image drift.
  browser = await chromium.launch({ executablePath: resolveChromium() });
  const page = await browser.newPage({
    viewport: { width: 1100, height: 900 },
    deviceScaleFactor: 2,
  });
  page.on('pageerror', (e) => console.log('[pageerror]', String(e).slice(0, 300)));

  await page.goto(`${BASE}/`, { waitUntil: 'networkidle' });

  // Connect over the WiFi transport pointed at this server's own mock node.
  // The "Mock Node" button targets a fixed dev port rather than this one.
  await page.getByRole('button', { name: /^WiFi$/ }).click();
  await page.waitForTimeout(400);
  await page.locator('input[type="text"]').first().fill(`127.0.0.1:${PORT}`);
  await page.getByRole('button', { name: /^Connect$/ }).click();

  await page.getByRole('button', { name: /^Stats$/ }).first().click({ timeout: 30_000 });
  await page.getByRole('button', { name: 'Start roll call' }).scrollIntoViewIfNeeded();

  await page.getByLabel('Roll-call message').fill('sound off');
  await page.getByRole('button', { name: 'Start roll call' }).click();

  // Wait for the last of the mock fleet's four answers (it lands at 6.32s)
  // rather than for a fixed sleep, so the shot always shows a full ledger.
  await page.getByText('6.3s in').waitFor({ timeout: 40_000 });
  await page.waitForTimeout(400);

  const card = page.getByTestId('rollcall-panel');
  await card.scrollIntoViewIfNeeded();
  await card.screenshot({ path: path.join(OUT, 'webapp-rollcall.png') });
  console.log('captured', path.join(OUT, 'webapp-rollcall.png'));

  await shutdown(0);
} catch (err) {
  console.error(err);
  await shutdown(1);
}
