// globalSetup.ts
//
// Boots the live stack ONCE for the whole suite: gosim (broker + UI static
// files), unscenarioed. Each spec loads whatever scenario it needs through
// the real UI, the same way a human does per emulator/README.md. Reusing one
// broker across specs (rather than a fresh process per test) keeps the whole
// suite's wall-clock budget well under the 4-minute target while still
// exercising the real process boundary (real firmware nodes spawned by real
// gosim) for every scenario load.

import { bootStack } from './lib/stack';

export default async function globalSetup() {
  const port = Number(process.env.E2E_PORT);
  if (!Number.isFinite(port) || port <= 0) {
    throw new Error('globalSetup: E2E_PORT must be set to a valid port (set by emulator/e2e/run_e2e.sh)');
  }
  const stack = await bootStack(port);
  console.log(`[e2e] gosim up: pid=${stack.pid} ${stack.baseURL}`);
}
