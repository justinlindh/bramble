// globalTeardown.ts
//
// Tears down whatever globalSetup.ts booted: kills the gosim process group
// (which takes any live firmware node children with it) and pkills the node
// binary path as a safety net, matching emulator/scripts/smoke_live.sh and
// emulator/ci/run_scenarios.sh's cleanup convention. Runs even if tests
// failed (Playwright always runs globalTeardown after globalSetup succeeded).

import { teardownStack } from './lib/stack';

export default async function globalTeardown() {
  await teardownStack();
  console.log('[e2e] stack torn down');
}
