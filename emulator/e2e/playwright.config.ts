// playwright.config.ts
//
// Browser-level acceptance suite for the Bramble emulator (Task 13). Runs
// against one shared live stack (globalSetup.ts / globalTeardown.ts): gosim
// broker + built UI + real firmware node processes spawned per scenario. All
// specs run serially against that one stack (workers: 1), since a scenario
// load resets the broker's whole simulation state -- concurrent specs would
// race each other's scenarios.
//
// node_modules resolution: this config and the specs under specs/ live
// outside simulator/ui (where @playwright/test is installed, reusing
// webapp/'s pinned version per the task brief) because Node's module
// resolution walks up from each importing FILE's own directory, not the
// invoking shell's cwd -- so emulator/e2e/node_modules is a symlink to
// simulator/ui/node_modules (see emulator/e2e/run_e2e.sh), making `import
// from '@playwright/test'` resolve normally from specs/*.spec.ts.

import { defineConfig, devices } from '@playwright/test';

const port = process.env.E2E_PORT ?? '3913';

export default defineConfig({
  testDir: './specs',
  fullyParallel: false,
  workers: 1,
  retries: 0,
  // Ceilings, not targets: every wait in the specs is event-driven, so a fast
  // box still finishes the whole suite in under a minute. The per-test ceiling
  // must exceed the delivery waits' 110s budget (which covers the scenario's
  // full send schedule on a CPU-contended CI pod); the old 60s/4min pair sat
  // below it and turned pod slowness into spurious failures.
  timeout: 240_000,
  globalTimeout: 15 * 60_000,
  globalSetup: require.resolve('./globalSetup'),
  globalTeardown: require.resolve('./globalTeardown'),
  reporter: [['list']],
  outputDir: './.run/test-results',
  use: {
    baseURL: `http://127.0.0.1:${port}`,
    screenshot: 'only-on-failure',
    trace: 'retain-on-failure',
    video: 'off',
  },
  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] },
    },
  ],
});
