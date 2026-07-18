// delivery-on-load.spec.ts
//
// Regression for the "devices show, but messages never get sent" bug.
//
// A real user loads a firmware scenario and expects it to work: the pagers
// boot, and a broadcast is delivered and rendered. Before the auto-start fix,
// delivery was silently gated behind a manual Play press -- a virtual-time
// harness concept that has no meaning for real-time firmware processes, which
// transmit on the wall clock the moment they attach. So a user who just loaded
// a scenario saw the pagers boot and then nothing, because the broker never
// ran the delivery pump. The other e2e specs mask this by clicking Play in
// their load helper; this spec deliberately does NOT, so it drives the exact
// flow a first-time user does and fails if delivery ever regresses to being
// play-gated for firmware scenarios.

import { test, expect } from '@playwright/test';
import { decodeFbWire } from '../lib/fbWire';
import { findText } from '../lib/glyphMatch';
import { attachWsCapture, waitFor, type WsCapture } from '../lib/wsCapture';
import { loadScenarioNoPlay } from '../lib/uiActions';

test('a freshly loaded firmware scenario delivers messages with no manual Play', async ({ page }) => {
  const cap: WsCapture = attachWsCapture(page);
  await page.goto('/');
  await page.getByTestId('view-tab-devices').click();

  // Load a sending scenario, then touch NOTHING. No Play button, ever.
  await loadScenarioNoPlay(page, 'emu-channel-delivery');

  // The three firmware nodes attach (spawned by the supervisor on load).
  await waitFor(() => (cap.distinctNodes().length >= 3 ? true : undefined), {
    timeoutMs: 30_000,
    label: '3 firmware nodes attach',
  });

  // The headline assertion: with no manual Play, the broadcast is still
  // delivered and at least two receivers render it on their e-paper. This is
  // the exact thing the user reported broken; it can only pass if the server
  // auto-starts a firmware scenario on load.
  const hits = await waitFor(
    () => {
      const rendered = new Set<string>();
      for (const ev of cap.fbEvents) {
        if (rendered.has(ev.node)) continue;
        if (findText(decodeFbWire(ev.fb), 'HELLO BRAMBLE', 1).found) rendered.add(ev.node);
      }
      return rendered.size >= 2 ? rendered : undefined;
    },
    // Covers the scenario's full send schedule (sender t=12s..100s) so a
    // CPU-contended pod cannot outrun the window; event-driven, so a fast box
    // exits as soon as both receivers have rendered (typically ~15s).
    { timeoutMs: 110_000, label: '>=2 nodes render "HELLO BRAMBLE" with no Play press' },
  );
  expect(
    hits.size,
    'distinct nodes that rendered the broadcast after a plain Load, no manual Play',
  ).toBeGreaterThanOrEqual(2);
});
