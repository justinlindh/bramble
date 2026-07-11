// globalTeardown.ts
//
// Tears down whatever globalSetup.ts booted: kills ONLY the gosim process
// group this run's globalSetup spawned (its recorded pid, see lib/stack.ts),
// which takes any live firmware node children with it. Scoped to this run's
// own pid rather than a name-wide pkill, so an unrelated gosim/firmware-node
// instance running the same binary on a different port (e.g. a developer's
// live `make run`) is left untouched. Runs even if tests failed (Playwright
// always runs globalTeardown after globalSetup succeeded).

import { teardownStack } from './lib/stack';

export default async function globalTeardown() {
  await teardownStack();
  console.log('[e2e] stack torn down');
}
