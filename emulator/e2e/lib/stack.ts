// stack.ts
//
// Boots and tears down the full live stack (gosim broker + UI static files;
// gosim itself spawns real firmware node processes once a scenario is loaded)
// for the Playwright suite. Mirrors the proven CWD/binary-path convention
// from emulator/scripts/smoke_live.sh and emulator/ci/run_scenarios.sh: gosim
// resolves each scenario's "binary" field (e.g.
// "emulator/node/build/bramble-node.elf") relative to ITS OWN process CWD
// (Go's exec.Command uses the path as-is for anything containing a "/"), so
// gosim is always spawned with cwd = repo root here, regardless of where
// `make e2e` / `playwright test` was invoked from.
//
// gosim is spawned detached in its own process group (setsid via
// `detached: true` on Linux) so teardown can kill the whole group in one
// shot -- gosim's own children (the firmware node processes it forks per
// scenario) inherit that group and die with it even if gosim's own SIGTERM
// handling doesn't proactively reap them.

import { spawn, execSync } from 'node:child_process';
import * as fs from 'node:fs';
import * as path from 'node:path';
import * as net from 'node:net';

export const REPO_ROOT = path.resolve(__dirname, '..', '..', '..');
export const GOSIM_BIN = path.join(REPO_ROOT, 'simulator', 'gosim', 'bramble-gosim');
export const UI_DIST = path.join(REPO_ROOT, 'simulator', 'ui', 'dist');
export const SCEN_DIR = path.join(REPO_ROOT, 'simulator', 'scenarios');
export const NODE_BIN = path.join(REPO_ROOT, 'emulator', 'node', 'build', 'bramble-node.elf');

const RUN_DIR = path.join(__dirname, '..', '.run');
const PID_FILE = path.join(RUN_DIR, 'gosim.pid');

export function findFreePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.unref();
    srv.on('error', reject);
    srv.listen(0, '127.0.0.1', () => {
      const addr = srv.address();
      if (addr && typeof addr === 'object') {
        const port = addr.port;
        srv.close(() => resolve(port));
      } else {
        srv.close();
        reject(new Error('findFreePort: no address'));
      }
    });
  });
}

async function waitForHttp(url: string, timeoutMs: number): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  let lastErr: unknown;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(url);
      if (res.ok) return;
      lastErr = new Error(`HTTP ${res.status}`);
    } catch (err) {
      lastErr = err;
    }
    await new Promise((r) => setTimeout(r, 200));
  }
  throw new Error(`waitForHttp: ${url} not ready after ${timeoutMs}ms (last error: ${String(lastErr)})`);
}

export interface BootedStack {
  port: number;
  pid: number;
  baseURL: string;
}

// bootStack starts gosim in live (non-headless) mode: broker + UI static
// files, bound to the given port. No scenario is preloaded; specs drive
// scenario loading themselves through the real ScenarioLoader UI, exactly as
// a human would per emulator/README.md's `make run` walkthrough.
export async function bootStack(port: number): Promise<BootedStack> {
  if (!fs.existsSync(GOSIM_BIN)) {
    throw new Error(`bootStack: gosim binary missing: ${GOSIM_BIN} (build it: cd simulator/gosim && go build -o bramble-gosim .)`);
  }
  if (!fs.existsSync(NODE_BIN)) {
    throw new Error(`bootStack: firmware node binary missing: ${NODE_BIN} (build it: cd emulator/node && idf.py build)`);
  }
  if (!fs.existsSync(UI_DIST)) {
    throw new Error(`bootStack: UI dist missing: ${UI_DIST} (build it: cd simulator/ui && npm run build)`);
  }

  fs.mkdirSync(RUN_DIR, { recursive: true });

  const child = spawn(GOSIM_BIN, ['--ui', UI_DIST, '--scenarios', SCEN_DIR, '--port', String(port)], {
    cwd: REPO_ROOT,
    detached: true, // own process group: teardown kills -pid to reap firmware node children too
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  child.unref();

  const logPath = path.join(RUN_DIR, 'gosim.log');
  const logStream = fs.createWriteStream(logPath, { flags: 'a' });
  child.stdout?.pipe(logStream);
  child.stderr?.pipe(logStream);

  if (child.pid === undefined) {
    throw new Error('bootStack: gosim failed to spawn (no pid)');
  }
  fs.writeFileSync(PID_FILE, String(child.pid));

  const baseURL = `http://127.0.0.1:${port}`;
  await waitForHttp(`${baseURL}/api/scenarios`, 20_000);

  return { port, pid: child.pid, baseURL };
}

// teardownStack kills the gosim process group (which takes its firmware node
// children with it) and, as a safety net matching smoke_live.sh /
// run_scenarios.sh, pkills anything still matching the node binary path in
// case a child escaped the group.
function isAlive(pid: number): boolean {
  try {
    process.kill(pid, 0); // probe: throws if the process is gone
    return true;
  } catch {
    return false;
  }
}

export async function teardownStack(): Promise<void> {
  if (fs.existsSync(PID_FILE)) {
    const pid = Number(fs.readFileSync(PID_FILE, 'utf-8').trim());
    if (Number.isFinite(pid) && pid > 0) {
      try {
        process.kill(-pid, 'SIGTERM');
      } catch {
        /* already gone */
      }
      // Give it a moment to exit cleanly, then force-kill anything standing.
      const deadline = Date.now() + 3000;
      while (Date.now() < deadline && isAlive(pid)) {
        await new Promise((r) => setTimeout(r, 100));
      }
      try {
        process.kill(-pid, 'SIGKILL');
      } catch {
        /* already gone */
      }
    }
    fs.rmSync(PID_FILE, { force: true });
  }
  try {
    execSync(`pkill -f ${JSON.stringify(NODE_BIN)}`, { stdio: 'ignore' });
  } catch {
    /* nothing matched, fine */
  }
  try {
    execSync(`pkill -f ${JSON.stringify(GOSIM_BIN)}`, { stdio: 'ignore' });
  } catch {
    /* nothing matched, fine */
  }
}
